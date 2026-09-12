#include "insights_bridge.h"

#include <stdint.h>
#include <string.h>

#include "runtime_observability.h"

#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "INSIGHTS_BRIDGE";
static bool s_initialized = false;


#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS

#include "app_events.h"
#include "esp_diagnostics.h"
#include "esp_diagnostics_metrics.h"
#include "esp_event.h"
#include "esp_insights.h"


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
static bool s_retry_pending = false;


/*
 * HTTPS auth material is intentionally supplied by a separate, private
 * integration. No key is stored in this repository or sdkconfig.defaults.
 */
const char *app_esp_insights_auth_key(void) __attribute__((weak));


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
    if (s_initialized) {
        return ESP_OK;
    }

    if (!insights_has_auth_key()) {
        ESP_LOGW(
            TAG,
            "ESP Insights requested but no private auth-key provider is installed; bridge remains inactive"
        );
        s_retry_pending = false;
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
         * resource failure may be retried from the APP_EVENT loop after STA
         * obtains an IP. Never retry from the default sys_evt callback. */
        s_retry_pending = true;
        return ESP_OK;
    }

#if defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    insights_register_metrics();
#endif

    s_initialized = true;
    s_retry_pending = false;
    ESP_LOGI(TAG, "ESP Insights bridge enabled with selected runtime metrics only");
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
        /* This callback runs on the dedicated APP_EVENT user loop, not the
         * system Wi-Fi event callback. The retry is therefore in service
         * context and never extends the default sys_evt callback. */
        insights_bridge_on_network_ready();
    }
}


#endif /* CONFIG_APP_ESP_INSIGHTS helper implementations */


esp_err_t insights_bridge_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS
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
    if (!s_retry_pending || s_initialized) {
        return;
    }

    /* Consume the request before entering the attempt. A failed attempt sets
     * it again, so a later APP_EVENT_WIFI_STA_CONNECTED event can retry
     * without re-entrancy. */
    s_retry_pending = false;
    (void)insights_bridge_attempt_init();
#endif
}


esp_err_t insights_bridge_publish_runtime(void)
{
#if defined(CONFIG_APP_ESP_INSIGHTS) && CONFIG_APP_ESP_INSIGHTS && \
    defined(CONFIG_DIAG_ENABLE_METRICS) && CONFIG_DIAG_ENABLE_METRICS
    if (!s_initialized) {
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
    return s_initialized;
}
