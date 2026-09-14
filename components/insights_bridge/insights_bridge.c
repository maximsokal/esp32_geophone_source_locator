#include "insights_bridge.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "runtime_observability.h"

#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "INSIGHTS_BRIDGE";
static atomic_bool s_initialized = false;


#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS

#include "app_events.h"
#include "app_task_config.h"
#include "esp_diagnostics.h"
#include "esp_diagnostics_metrics.h"
#include "esp_event.h"
#include "esp_insights.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/*
 * ESP Insights also enables the diagnostics component's built-in variables
 * and optional heap/Wi-Fi metric producers. They include network identity
 * such as SSID/BSSID, which is outside this application's selected health
 * metric contract. The dedicated enabled profile turns those producers off;
 * fail the opt-in build if a caller enables them accidentally.
 */
#if defined(CONFIG_DIAG_ENABLE_VARIABLES) && CONFIG_DIAG_ENABLE_VARIABLES
#error "GeoLocator ESP Insights requires CONFIG_DIAG_ENABLE_VARIABLES=n"
#endif
#if (defined(CONFIG_DIAG_ENABLE_HEAP_METRICS) && CONFIG_DIAG_ENABLE_HEAP_METRICS) || \
    (defined(CONFIG_DIAG_ENABLE_WIFI_METRICS) && CONFIG_DIAG_ENABLE_WIFI_METRICS)
#error "GeoLocator ESP Insights requires built-in heap and Wi-Fi metrics to be disabled"
#endif


static bool s_network_handler_registered = false;
static atomic_bool s_retry_pending = false;
static TaskHandle_t s_retry_task = NULL;


/*
 * HTTPS auth material is intentionally supplied by a separate, private
 * integration. No key is stored in this repository or sdkconfig.defaults.
 * ESP-IDF uses GCC/Clang, where the weak hook lets a private integration
 * override the symbol. The native MSVC host contract does not use HTTPS and
 * only needs the declaration to remain parseable.
 */
#if defined(__GNUC__) || defined(__clang__)
#define INSIGHTS_BRIDGE_WEAK __attribute__((weak))
#else
#define INSIGHTS_BRIDGE_WEAK
#endif
const char *app_esp_insights_auth_key(void) INSIGHTS_BRIDGE_WEAK;
#undef INSIGHTS_BRIDGE_WEAK


static bool insights_has_auth_key(void)
{
#if defined(CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS) && CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
    if (app_esp_insights_auth_key == NULL) {
        return false;
    }

    const char *key = app_esp_insights_auth_key();
    return key != NULL && key[0] != '\0';
#else
    return true;
#endif
}


#if defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
static void insights_register_metrics(void)
{
    (void)esp_diag_metrics_register(
        "runtime", "internal_free", "Internal free bytes", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "internal_largest", "Internal largest block", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "psram_free", "PSRAM free bytes", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "psram_largest", "PSRAM largest block", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "acquisition_heartbeat", "Acquisition heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "recorder_heartbeat", "Recorder heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
    (void)esp_diag_metrics_register(
        "runtime", "ui_heartbeat", "UI heartbeat", "runtime",
        ESP_DIAG_DATA_TYPE_UINT
    );
}
#endif

#endif /* optional ESP Insights declarations */


#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS

static void insights_bridge_app_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);


static esp_err_t insights_bridge_register_network_ready_handler(void)
{
    if (s_network_handler_registered) {
        return ESP_OK;
    }

    esp_event_loop_handle_t loop =
        app_events_get_loop();
    if (loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        esp_event_handler_register_with(
            loop,
            APP_EVENT,
            APP_EVENT_WIFI_STA_CONNECTED,
            insights_bridge_app_event_handler,
            NULL
        );

    if (err == ESP_OK) {
        s_network_handler_registered = true;
    }

    return err;
}


static esp_err_t insights_bridge_attempt_init(void)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_OK;
    }

    if (!insights_has_auth_key()) {
        ESP_LOGW(
            TAG,
            "ESP Insights requested but no private auth-key provider is installed; bridge remains inactive"
        );
        atomic_store_explicit(
            &s_retry_pending,
            false,
            memory_order_release
        );
        return ESP_OK;
    }

    esp_insights_config_t config = {
        /* Metrics only: application logs may contain device-specific data. */
        .log_type = 0U,
    };

#if defined(CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS) && CONFIG_ESP_INSIGHTS_TRANSPORT_HTTPS
    config.auth_key = app_esp_insights_auth_key();
#endif

    /* The agent is additive and its failure must never block device boot. */
    const esp_err_t init_err = esp_insights_init(&config);
    if (init_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "ESP Insights initialization unavailable; continuing without it: %s",
            esp_err_to_name(init_err)
        );
        /* The platform is ready at the normal call site, but a transient
         * resource failure may be retried after STA obtains an IP. The
         * APP_EVENT callback only notifies the dedicated retry worker. */
        atomic_store_explicit(
            &s_retry_pending,
            true,
            memory_order_release
        );
        return ESP_OK;
    }

#if defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    insights_register_metrics();
#endif

    atomic_store_explicit(
        &s_initialized,
        true,
        memory_order_release
    );
    atomic_store_explicit(
        &s_retry_pending,
        false,
        memory_order_release
    );
    ESP_LOGI(TAG, "ESP Insights bridge enabled with selected runtime metrics only");
    return ESP_OK;
}


static void insights_bridge_retry_worker(void *context)
{
    (void)context;

    for (;;) {
        (void)ulTaskNotifyTake(
            pdTRUE,
            portMAX_DELAY
        );

        if (
            atomic_load_explicit(&s_initialized, memory_order_acquire) ||
            !atomic_load_explicit(&s_retry_pending, memory_order_acquire)
        ) {
            continue;
        }

        /* Consume the request before entering the attempt. A failed attempt
         * sets it again, and the next connected event will notify us again. */
        atomic_store_explicit(
            &s_retry_pending,
            false,
            memory_order_release
        );
        (void)insights_bridge_attempt_init();
    }
}


static esp_err_t insights_bridge_start_retry_worker(void)
{
    if (s_retry_task != NULL) {
        return ESP_OK;
    }

    const BaseType_t result =
        xTaskCreatePinnedToCore(
            insights_bridge_retry_worker,
            "insights_retry",
            APP_TASK_STACK_INSIGHTS,
            NULL,
            APP_TASK_PRIORITY_INSIGHTS,
            &s_retry_task,
            APP_TASK_CORE_SERVICES
        );

    if (result != pdPASS) {
        s_retry_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


static void insights_bridge_app_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_data;

    if (
        event_base == APP_EVENT &&
        event_id == APP_EVENT_WIFI_STA_CONNECTED
    ) {
        /* The APP_EVENT loop is shared with UI/application subscribers. Only
         * notify the dedicated worker and return; Insights initialization is
         * intentionally outside every event callback. */
        insights_bridge_on_network_ready();
    }
}


#endif /* CONFIG_APP_ESP_INSIGHTS helper implementations */


esp_err_t insights_bridge_init(void)
{
    if (atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_OK;
    }

#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS
    const esp_err_t worker_err =
        insights_bridge_start_retry_worker();
    if (worker_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "ESP Insights retry worker unavailable: %s",
            esp_err_to_name(worker_err)
        );
    }

    const esp_err_t handler_err =
        insights_bridge_register_network_ready_handler();
    if (handler_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "ESP Insights retry handler unavailable: %s",
            esp_err_to_name(handler_err)
        );
    }

    return insights_bridge_attempt_init();
#else
    ESP_LOGI(TAG, "ESP Insights bridge disabled (CONFIG_APP_ESP_INSIGHTS=n)");
    return ESP_OK;
#endif
}


void insights_bridge_on_network_ready(void)
{
#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS
    if (s_retry_task == NULL) {
        return;
    }

    (void)xTaskNotifyGive(s_retry_task);
#endif
}


esp_err_t insights_bridge_publish_runtime(void)
{
#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS && \
    defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    if (!atomic_load_explicit(&s_initialized, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    runtime_observability_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    runtime_observability_get_snapshot(&snapshot);

    (void)esp_diag_metrics_add_uint(
        "internal_free",
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    (void)esp_diag_metrics_add_uint(
        "internal_largest",
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    (void)esp_diag_metrics_add_uint(
        "psram_free",
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );
    (void)esp_diag_metrics_add_uint(
        "psram_largest",
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
    );
    (void)esp_diag_metrics_add_uint(
        "acquisition_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_ACQUISITION].heartbeat
    );
    (void)esp_diag_metrics_add_uint(
        "recorder_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_RECORDER].heartbeat
    );
    (void)esp_diag_metrics_add_uint(
        "ui_heartbeat",
        (uint32_t)snapshot.tasks[RUNTIME_TASK_UI].heartbeat
    );
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


bool insights_bridge_enabled(void)
{
    return atomic_load_explicit(&s_initialized, memory_order_acquire);
}
