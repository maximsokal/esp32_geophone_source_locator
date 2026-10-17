#include "web_server.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "app_config.h"
#include "app_task_config.h"
#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "runtime_observability.h"
#include "measurement_service.h"
#include "object_service.h"
#include "object_store.h"
#include "web_mdns_retry_policy.h"
#include "web_server_lifecycle_policy.h"
#include "web_server_internal.h"
#include "web_route_capacity.h"
#include "web_cjson_hooks.h"

#define WEB_TLS_FATAL_ALERT_CODE 0x7780
#define WEB_TLS_HANDSHAKE_TIMEOUT_MS 5000U
#define WEB_HTTPS_TASK_STACK_BYTES   8192U
#define WEB_HTTPS_MAX_CLIENTS           2U
#define WEB_LIFECYCLE_QUIESCE_TIMEOUT_MS 10000U

static const char *TAG = "WEB_SERVER";

esp_err_t web_nrf_logs_api_register(httpd_handle_t server);

typedef struct {
    web_lifecycle_state_t state;
    uint32_t generation;
} web_lifecycle_command_t;

static httpd_handle_t s_http_server = NULL;
static httpd_handle_t s_https_server = NULL;
static QueueHandle_t s_lifecycle_queue = NULL;
static SemaphoreHandle_t s_lifecycle_lock = NULL;
static TaskHandle_t s_lifecycle_task = NULL;
static SemaphoreHandle_t s_lifecycle_quiesce_ack = NULL;
static bool s_lifecycle_stop_requested = false;
static bool s_lifecycle_quiesce_pending = false;
static bool s_lifecycle_transition_active = false;
static bool s_lifecycle_deferred_state_valid = false;
static web_lifecycle_state_t s_lifecycle_deferred_state;
static web_lifecycle_state_t s_lifecycle_stable_state =
    WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
static web_lifecycle_state_t s_lifecycle_rollback_state =
    WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
static unsigned s_lifecycle_quiesce_callers = 0U;
static esp_err_t s_lifecycle_quiesce_result = ESP_FAIL;
static bool s_event_handlers_registered = false;
static bool s_measurement_hooks_registered = false;
static bool s_object_observer_registered = false;
static portMUX_TYPE s_https_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_https_clients = 0U;
static uint32_t s_https_max_clients = 0U;
static uint32_t s_https_handshake_failures = 0U;
static uint32_t s_peer_fatal_alerts = 0U;
static int32_t s_https_last_error_esp_err = 0;
static uint32_t s_lifecycle_generation = 0U;
static web_mdns_retry_policy_t s_mdns_retry_policy;

void web_status_json_objects_object_event(
    const object_service_event_t *event,
    void *user_context
);

#if WEB_SERVER_TLS_CREDENTIALS_EMBEDDED
extern const unsigned char servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const unsigned char servercert_pem_end[] asm("_binary_servercert_pem_end");
extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
extern const unsigned char prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");
#endif

void web_server_https_runtime_status(
    uint32_t *clients_out,
    uint32_t *max_clients_out,
    uint32_t *handshake_failures_out,
    uint32_t *fatal_alerts_out,
    int32_t *last_error_esp_err_out
)
{
    portENTER_CRITICAL(&s_https_stats_lock);
    if (clients_out != NULL) *clients_out = s_https_clients;
    if (max_clients_out != NULL) *max_clients_out = s_https_max_clients;
    if (handshake_failures_out != NULL) *handshake_failures_out = s_https_handshake_failures;
    if (fatal_alerts_out != NULL) *fatal_alerts_out = s_peer_fatal_alerts;
    if (last_error_esp_err_out != NULL) *last_error_esp_err_out = s_https_last_error_esp_err;
    portEXIT_CRITICAL(&s_https_stats_lock);
}

static void web_server_log_tls_heap(const char *stage)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t clients = 0U;
    uint32_t fatal_alerts = 0U;
    web_server_https_runtime_status(&clients, NULL, NULL, &fatal_alerts, NULL);
    ESP_LOGI(
        TAG,
        "TLS_HEAP %s clients=%u free=%u largest=%u peer_alerts=%u",
        stage,
        (unsigned)clients,
        (unsigned)free_internal,
        (unsigned)largest_internal,
        (unsigned)fatal_alerts
    );
}

static bool web_server_tls_credentials_apply(httpd_ssl_config_t *config)
{
    if (config == NULL) return false;
#if WEB_SERVER_TLS_CREDENTIALS_EMBEDDED
    config->servercert = servercert_pem_start;
    config->servercert_len = servercert_pem_end - servercert_pem_start;
    config->prvtkey_pem = prvtkey_pem_start;
    config->prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;
    return true;
#else
    return false;
#endif
}

static esp_err_t web_server_https_stop_internal(void);
static esp_err_t web_server_http_stop_internal(void);
static esp_err_t web_server_http_start_internal(void);
static esp_err_t web_server_lifecycle_quiesce(void *context);
static void web_server_lifecycle_ack_quiesce(esp_err_t result);
static void web_server_lifecycle_radio_stop_complete(
    void *context,
    esp_err_t radio_stop_result
);
static void web_server_mode_transition_complete(
    void *context,
    bool running,
    bool setup_mode,
    esp_err_t transition_result,
    esp_err_t cleanup_result
);

static uint32_t web_server_lifecycle_next_generation_locked(void)
{
    const uint32_t next_generation = s_lifecycle_generation + 1U;
    return next_generation == 0U ? 1U : next_generation;
}

static BaseType_t web_server_lifecycle_queue_state_locked(web_lifecycle_state_t state)
{
    if (s_lifecycle_queue == NULL) return pdFALSE;

    const uint32_t generation = web_server_lifecycle_next_generation_locked();
    const web_lifecycle_command_t command = { .state = state, .generation = generation };
    const BaseType_t queued = xQueueOverwrite(s_lifecycle_queue, &command);
    if (queued == pdTRUE) {
        s_lifecycle_generation = generation;
        web_mdns_retry_policy_cancel(&s_mdns_retry_policy, generation);
    }
    return queued;
}


static esp_err_t web_server_lifecycle_reconcile_deferred_locked(void)
{
    if (!s_lifecycle_deferred_state_valid) {
        return ESP_OK;
    }

    if (
        s_lifecycle_stop_requested ||
        s_lifecycle_queue == NULL ||
        s_lifecycle_task == NULL
    ) {
        s_lifecycle_deferred_state_valid = false;
        return ESP_OK;
    }

    const BaseType_t queued = web_server_lifecycle_queue_state_locked(
        s_lifecycle_deferred_state
    );
    if (queued != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_lifecycle_stable_state = s_lifecycle_deferred_state;
    s_lifecycle_deferred_state_valid = false;
    return ESP_OK;
}


static esp_err_t web_server_lifecycle_reconcile_failed_transition_locked(void)
{
    if (s_lifecycle_deferred_state_valid) {
        return web_server_lifecycle_reconcile_deferred_locked();
    }

    if (
        s_lifecycle_stop_requested ||
        s_lifecycle_queue == NULL ||
        s_lifecycle_task == NULL
    ) {
        return ESP_OK;
    }

    const BaseType_t queued = web_server_lifecycle_queue_state_locked(
        s_lifecycle_rollback_state
    );
    if (queued != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_lifecycle_stable_state = s_lifecycle_rollback_state;
    return ESP_OK;
}


static esp_err_t web_server_lifecycle_set_desired_state_locked(
    web_lifecycle_state_t desired_state
)
{
    const bool ready =
        s_lifecycle_queue != NULL &&
        s_lifecycle_task != NULL;
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lifecycle_stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lifecycle_transition_active) {
        s_lifecycle_deferred_state = desired_state;
        s_lifecycle_deferred_state_valid = true;
        return ESP_OK;
    }

    if (s_lifecycle_quiesce_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = web_server_lifecycle_queue_state_locked(desired_state);

    if (result == pdTRUE) {
        s_lifecycle_stable_state = desired_state;
    }

    return result == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}


static esp_err_t web_server_lifecycle_set_desired_state(
    web_lifecycle_state_t desired_state
)
{
    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, 0) != pdTRUE
    ) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err =
        web_server_lifecycle_set_desired_state_locked(desired_state);
    (void)xSemaphoreGive(s_lifecycle_lock);
    return err;
}


static esp_err_t web_server_lifecycle_set_authoritative_state(
    web_lifecycle_state_t desired_state
)
{
    /* Mode completion runs in a normal task after the radio transaction. Do
     * not drop this final state merely because the lifecycle worker briefly
     * owns the mutex. */
    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE
    ) {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t err =
        web_server_lifecycle_set_desired_state_locked(desired_state);
    (void)xSemaphoreGive(s_lifecycle_lock);
    return err;
}


static esp_err_t web_server_lifecycle_set_network_event_state(
    web_lifecycle_state_t event_state
)
{
    /* The safety decision and queue commit must share this mutex. Otherwise
     * a transition can publish authoritative DISCONNECTED after the check
     * but before this event overwrites the one-slot queue. */
    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE
    ) {
        return ESP_ERR_TIMEOUT;
    }

    const web_lifecycle_state_t desired_state =
        web_lifecycle_effective_network_state(
            event_state,
            wifi_manager_is_radio_state_uncertain()
        );
    const esp_err_t err =
        web_server_lifecycle_set_desired_state_locked(desired_state);
    (void)xSemaphoreGive(s_lifecycle_lock);
    return err;
}


static void web_server_mode_transition_complete(
    void *context,
    bool running,
    bool setup_mode,
    esp_err_t transition_result,
    esp_err_t cleanup_result
)
{
    (void)context;

    web_lifecycle_state_t desired_state =
        WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
    if (cleanup_result == ESP_OK && running && setup_mode) {
        desired_state = WEB_LIFECYCLE_STATE_SETUP_AP;
    }
    else if (cleanup_result == ESP_OK && running) {
        char sta_ip[16];
        if (wifi_manager_sta_ip_string(sta_ip, sizeof(sta_ip)) == ESP_OK) {
            desired_state = WEB_LIFECYCLE_STATE_STA_CONNECTED;
        }
    }

    if (cleanup_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Wi-Fi mode transition left radio state uncertain: target=%s cleanup=%s",
            esp_err_to_name(transition_result),
            esp_err_to_name(cleanup_result)
        );
    }

    const esp_err_t err =
        web_server_lifecycle_set_authoritative_state(desired_state);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to reconcile web lifecycle after Wi-Fi mode transition target=%s cleanup=%s: %s",
            esp_err_to_name(transition_result),
            esp_err_to_name(cleanup_result),
            esp_err_to_name(err)
        );
    }
}


static esp_err_t web_server_lifecycle_quiesce(void *context)
{
    (void)context;

    SemaphoreHandle_t ack = xSemaphoreCreateBinary();
    if (ack == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, 0) != pdTRUE
    ) {
        vSemaphoreDelete(ack);
        return ESP_ERR_TIMEOUT;
    }

    const bool ready =
        s_lifecycle_queue != NULL &&
        s_lifecycle_task != NULL &&
        !s_lifecycle_stop_requested &&
        !s_lifecycle_transition_active &&
        !s_lifecycle_quiesce_pending &&
        s_lifecycle_quiesce_ack == NULL;
    if (!ready) {
        (void)xSemaphoreGive(s_lifecycle_lock);
        vSemaphoreDelete(ack);
        return ESP_ERR_INVALID_STATE;
    }

    s_lifecycle_quiesce_ack = ack;
    s_lifecycle_quiesce_result = ESP_FAIL;
    s_lifecycle_quiesce_pending = true;
    s_lifecycle_transition_active = true;
    s_lifecycle_deferred_state_valid = false;
    s_lifecycle_rollback_state = s_lifecycle_stable_state;
    ++s_lifecycle_quiesce_callers;

    /*
     * HTTP is intentionally not part of this barrier. The setup endpoint
     * can request the AP -> STA transition from the HTTP server task itself;
     * stopping that server here would wait on the caller being serviced.
     * The subsequent connected-state plan stops HTTP after the transition.
     */
    const BaseType_t queued = web_server_lifecycle_queue_state_locked(
        WEB_LIFECYCLE_STATE_QUIESCED
    );
    if (queued != pdTRUE) {
        s_lifecycle_quiesce_pending = false;
        s_lifecycle_transition_active = false;
        s_lifecycle_quiesce_ack = NULL;
        --s_lifecycle_quiesce_callers;
        const esp_err_t reconcile_err =
            web_server_lifecycle_reconcile_failed_transition_locked();
        if (reconcile_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to restore web lifecycle after quiesce queue failure: %s",
                esp_err_to_name(reconcile_err)
            );
        }
        (void)xSemaphoreGive(s_lifecycle_lock);
        vSemaphoreDelete(ack);
        return ESP_ERR_TIMEOUT;
    }

    (void)xSemaphoreGive(s_lifecycle_lock);

    const BaseType_t acknowledged = xSemaphoreTake(
        ack,
        pdMS_TO_TICKS(WEB_LIFECYCLE_QUIESCE_TIMEOUT_MS)
    );

    esp_err_t result = ESP_ERR_TIMEOUT;
    (void)xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    if (acknowledged == pdTRUE) {
        result = s_lifecycle_quiesce_result;
    }
    if (s_lifecycle_quiesce_ack == ack) {
        s_lifecycle_quiesce_ack = NULL;
    }
    if (acknowledged != pdTRUE || result != ESP_OK) {
        s_lifecycle_quiesce_pending = false;
        s_lifecycle_transition_active = false;
        const esp_err_t reconcile_err =
            web_server_lifecycle_reconcile_failed_transition_locked();
        if (reconcile_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to restore web lifecycle after radio-stop prepare failure: %s",
                esp_err_to_name(reconcile_err)
            );
        }
    }
    --s_lifecycle_quiesce_callers;
    (void)xSemaphoreGive(s_lifecycle_lock);

    vSemaphoreDelete(ack);
    return result;
}


static void web_server_lifecycle_ack_quiesce(esp_err_t result)
{
    if (s_lifecycle_lock == NULL) {
        return;
    }

    (void)xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    if (s_lifecycle_quiesce_pending && s_lifecycle_quiesce_ack != NULL) {
        s_lifecycle_quiesce_result = result;
        /* The worker is quiesced, but the radio transition is still active. */
        s_lifecycle_quiesce_pending = false;
        (void)xSemaphoreGive(s_lifecycle_quiesce_ack);
    }
    (void)xSemaphoreGive(s_lifecycle_lock);
}


static void web_server_lifecycle_radio_stop_complete(
    void *context,
    esp_err_t radio_stop_result
)
{
    (void)context;

    if (s_lifecycle_lock == NULL) {
        return;
    }

    (void)xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    if (!s_lifecycle_transition_active) {
        (void)xSemaphoreGive(s_lifecycle_lock);
        return;
    }

    s_lifecycle_transition_active = false;
    const esp_err_t reconcile_err =
        radio_stop_result == ESP_OK
            ? web_server_lifecycle_reconcile_deferred_locked()
            : web_server_lifecycle_reconcile_failed_transition_locked();
    (void)xSemaphoreGive(s_lifecycle_lock);

    if (radio_stop_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Wi-Fi radio stop completed with error: %s",
            esp_err_to_name(radio_stop_result)
        );
    }
    if (reconcile_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to reconcile web lifecycle after radio stop: %s",
            esp_err_to_name(reconcile_err)
        );
    }
}


static void web_server_lifecycle_unregister_wifi_hook(void)
{
    const esp_err_t err =
        wifi_manager_set_radio_stop_lifecycle_callbacks(NULL, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to unregister Wi-Fi lifecycle hook: %s",
            esp_err_to_name(err)
        );
    }
}


static void web_server_unregister_mode_transition_callback(void)
{
    const esp_err_t err =
        wifi_manager_set_mode_transition_complete_callback(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to unregister Wi-Fi mode transition callback: %s",
            esp_err_to_name(err)
        );
    }
}


static esp_err_t web_server_measurement_select_object(
    const char *object_id,
    void *user_context
)
{
    (void)user_context;

    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = object_store_set_active(object_id);
    if (err == ESP_OK) {
        web_status_json_objects_set_active(object_id);
    }
    return err;
}


static void web_server_status_cache_start(void)
{
    esp_err_t err = web_status_json_objects_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status object/session cache unavailable: %s", esp_err_to_name(err));
    }
    else {
        /*
         * Do not make network-server startup wait for an SD directory walk or
         * metadata reads. The cache task owns the refresh and /api/status can
         * report storage_busy while its first snapshot is being built.
         */
        err = web_status_json_objects_request_refresh();
        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Initial status object/session cache refresh request failed: %s",
                esp_err_to_name(err)
            );
        }
    }

    const esp_err_t guard_err = measurement_service_set_start_guard(
        web_status_json_objects_start_guard,
        NULL
    );
    const esp_err_t object_start_err =
        measurement_service_set_start_object_callback(
            web_server_measurement_select_object,
            NULL
        );
    const esp_err_t release_err =
        measurement_service_set_start_reservation_release_callback(
            web_status_json_objects_release_start_reservation,
            NULL
        );
    const esp_err_t refresh_err =
        measurement_service_set_storage_refresh_callback(
            web_status_json_objects_storage_changed,
            NULL
        );
    const esp_err_t object_observer_err = object_service_set_event_observer(
        web_status_json_objects_object_event,
        NULL
    );
    s_measurement_hooks_registered =
        guard_err == ESP_OK ||
        object_start_err == ESP_OK ||
        release_err == ESP_OK ||
        refresh_err == ESP_OK;
    s_object_observer_registered = object_observer_err == ESP_OK;
    if (
        guard_err != ESP_OK ||
        object_start_err != ESP_OK ||
        release_err != ESP_OK ||
        refresh_err != ESP_OK ||
        object_observer_err != ESP_OK
    ) {
        ESP_LOGW(
            TAG,
            "Status-cache hooks unavailable guard=%s object_start=%s release=%s refresh=%s object_observer=%s",
            esp_err_to_name(guard_err),
            esp_err_to_name(object_start_err),
            esp_err_to_name(release_err),
            esp_err_to_name(refresh_err),
            esp_err_to_name(object_observer_err)
        );
    }
}


static void web_server_status_cache_stop(void)
{
    if (s_measurement_hooks_registered) {
        (void)measurement_service_set_start_guard(NULL, NULL);
        (void)measurement_service_set_start_object_callback(NULL, NULL);
        (void)measurement_service_set_start_reservation_release_callback(NULL, NULL);
        (void)measurement_service_set_storage_refresh_callback(NULL, NULL);
        s_measurement_hooks_registered = false;
    }
    if (s_object_observer_registered) {
        (void)object_service_set_event_observer(NULL, NULL);
        s_object_observer_registered = false;
    }
    web_status_json_objects_shutdown();
}

static esp_err_t web_server_https_start_internal(void)
{
    if (s_https_server != NULL) return ESP_OK;

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.httpd.core_id = APP_TASK_CORE_WEB_SERVER;
    config.httpd.task_priority = APP_TASK_PRIORITY_WEB_SERVER;
    config.httpd.max_uri_handlers = WEB_HTTPS_URI_HANDLERS_CAPACITY;
    config.httpd.uri_match_fn = httpd_uri_match_wildcard;

    /*
     * The device has plenty of PSRAM but very little contiguous internal DRAM
     * after Wi-Fi, LVGL and the HTTPS task are running. The field log showed
     * one successful TLS client reducing internal free RAM from ~13.8 KiB to
     * ~4.4 KiB; Chrome's second speculative TLS socket then failed during
     * session creation and mDNS also started failing allocations.
     *
     * Serve the local dashboard over one persistent HTTP/1.1 TLS connection.
     * Browsers can serialize asset requests on it. LRU purge and a short read
     * timeout prevent an abandoned speculative socket from monopolizing the
     * device indefinitely. Reducing the HTTPD task stack from the ESP-IDF
     * HTTPS default (10 KiB) to 8 KiB also returns 2 KiB of internal DRAM.
     */
    config.httpd.max_open_sockets = WEB_HTTPS_MAX_CLIENTS;
    config.httpd.stack_size = WEB_HTTPS_TASK_STACK_BYTES;
    config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 3;
    config.httpd.send_wait_timeout = 30;
    config.port_secure = APP_WEB_HTTPS_PORT;

    /*
     * ESP-IDF 5.5.4 supports RFC5077 server tickets directly in
     * httpd_ssl_config_t. They do not bypass certificate verification; they
     * only make later sockets cheap after one trusted full handshake succeeds.
     */
    config.session_tickets = true;
    config.tls_handshake_timeout_ms = WEB_TLS_HANDSHAKE_TIMEOUT_MS;

    if (!web_server_tls_credentials_apply(&config)) {
        ESP_LOGW(TAG, "HTTPS disabled: local TLS credentials are not embedded");
        return ESP_ERR_NOT_FOUND;
    }

    portENTER_CRITICAL(&s_https_stats_lock);
    s_peer_fatal_alerts = 0U;
    portEXIT_CRITICAL(&s_https_stats_lock);
    runtime_observability_request_snapshot("before-https");
    web_server_log_tls_heap("before_start");
    ESP_LOGI(
        TAG,
        "URI handlers required=%u capacity=%u",
        (unsigned)WEB_HTTPS_URI_HANDLERS_REQUIRED,
        (unsigned)config.httpd.max_uri_handlers
    );
    ESP_LOGI(
        TAG,
        "Starting HTTPS server on port %u (max clients=%u, stack=%u, LRU=on, session tickets=on, handshake timeout=%u ms, core=%d, priority=%u)",
        (unsigned)config.port_secure,
        (unsigned)config.httpd.max_open_sockets,
        (unsigned)config.httpd.stack_size,
        (unsigned)config.tls_handshake_timeout_ms,
        (int)config.httpd.core_id,
        (unsigned)config.httpd.task_priority
    );

    esp_err_t err = httpd_ssl_start(&s_https_server, &config);
    if (err != ESP_OK) {
        s_https_server = NULL;
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(err));
        return err;
    }

    const esp_err_t webui_cache_err = web_webui_flash_prepare_cache();
    if (
        webui_cache_err != ESP_OK &&
        webui_cache_err != ESP_ERR_NOT_SUPPORTED &&
        webui_cache_err != ESP_ERR_NO_MEM
    ) {
        ESP_LOGW(
            TAG,
            "Optional WebUI PSRAM cache unavailable: %s",
            esp_err_to_name(webui_cache_err)
        );
    }

    web_server_route_tracking_begin(config.httpd.max_uri_handlers);
    err = web_server_routes_register(s_https_server);
    if (err == ESP_OK) err = web_nrf_logs_api_register(s_https_server);
    if (err == ESP_OK) {
        size_t route_count = 0U;
        const web_route_identity_t *routes = web_https_route_catalog(&route_count);
        err = web_server_route_tracking_finish(routes, route_count);
    }
    if (err != ESP_OK) {
        (void)web_server_https_stop_internal();
        return err;
    }

    web_server_log_tls_heap("after_start");
    runtime_observability_request_snapshot("after-https");
    ESP_LOGI(TAG, "HTTPS server started: %s", APP_MDNS_WEB_URL);
    return ESP_OK;
}

static esp_err_t web_server_https_stop_internal(void)
{
    if (s_https_server == NULL) {
        portENTER_CRITICAL(&s_https_stats_lock);
        s_https_clients = 0U;
        portEXIT_CRITICAL(&s_https_stats_lock);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping HTTPS server");
    esp_err_t err = httpd_ssl_stop(s_https_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTPS server: %s", esp_err_to_name(err));
        return err;
    }

    s_https_server = NULL;
    portENTER_CRITICAL(&s_https_stats_lock);
    s_https_clients = 0U;
    s_peer_fatal_alerts = 0U;
    portEXIT_CRITICAL(&s_https_stats_lock);
    ESP_LOGI(TAG, "HTTPS server stopped");
    return ESP_OK;
}

static esp_err_t web_server_http_start_internal(void)
{
    if (s_http_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = APP_TASK_CORE_WEB_SERVER;
    config.task_priority = APP_TASK_PRIORITY_WEB_SERVER;
    config.server_port = APP_WEB_HTTP_PORT;
    config.max_uri_handlers = WEB_HTTP_URI_HANDLERS_CAPACITY;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "URI handlers required=%u capacity=%u",
             (unsigned)WEB_HTTP_URI_HANDLERS_REQUIRED, (unsigned)config.max_uri_handlers);
    ESP_LOGI(TAG, "Starting HTTP bootstrap server on port %u core=%d priority=%u",
             (unsigned)config.server_port, (int)config.core_id, (unsigned)config.task_priority);
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        s_http_server = NULL;
        ESP_LOGE(TAG, "Failed to start HTTP bootstrap server: %s", esp_err_to_name(err));
        return err;
    }

    web_server_route_tracking_begin(config.max_uri_handlers);
    err = web_server_bootstrap_routes_register(s_http_server);
    if (err == ESP_OK) {
        size_t route_count = 0U;
        const web_route_identity_t *routes = web_http_route_catalog(&route_count);
        err = web_server_route_tracking_finish(routes, route_count);
    }
    if (err != ESP_OK) {
        (void)httpd_stop(s_http_server);
        s_http_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP bootstrap server started");
    return ESP_OK;
}

static esp_err_t web_server_http_stop_internal(void)
{
    if (s_http_server == NULL) return ESP_OK;

    ESP_LOGI(TAG, "Stopping HTTP bootstrap server");
    esp_err_t err = httpd_stop(s_http_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP bootstrap server: %s", esp_err_to_name(err));
        return err;
    }

    s_http_server = NULL;
    ESP_LOGI(TAG, "HTTP bootstrap server stopped");
    return ESP_OK;
}

static uint64_t web_server_lifecycle_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void web_server_mdns_retry_record_result(uint32_t generation, esp_err_t result)
{
    uint32_t next_delay_ms = 0U;
    uint32_t retry_count = 0U;
    bool stop_stale_mdns = false;

    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE
    ) {
        return;
    }

    const bool current_connected_generation =
        s_lifecycle_generation == generation &&
        s_lifecycle_stable_state == WEB_LIFECYCLE_STATE_STA_CONNECTED &&
        !s_lifecycle_transition_active &&
        !s_lifecycle_quiesce_pending &&
        !s_lifecycle_stop_requested;
    if (current_connected_generation) {
        if (result == ESP_OK) {
            web_mdns_retry_policy_reset_success(&s_mdns_retry_policy, generation);
        }
        else {
            next_delay_ms = web_mdns_retry_policy_schedule_failure(
                &s_mdns_retry_policy,
                generation,
                web_server_lifecycle_now_ms()
            );
            retry_count = s_mdns_retry_policy.retry_count;
        }
    }
    else if (
        result == ESP_OK &&
        s_lifecycle_stable_state != WEB_LIFECYCLE_STATE_STA_CONNECTED
    ) {
        stop_stale_mdns = true;
    }

    (void)xSemaphoreGive(s_lifecycle_lock);

    if (stop_stale_mdns) wifi_manager_mdns_stop();
    if (next_delay_ms > 0U) {
        ESP_LOGW(TAG, "mDNS retry %u scheduled in %u ms", (unsigned)retry_count, (unsigned)next_delay_ms);
    }
}

static bool web_server_mdns_retry_wait(uint32_t *generation_out, uint32_t *delay_ms_out)
{
    if (generation_out == NULL || delay_ms_out == NULL || s_lifecycle_lock == NULL) return false;
    *generation_out = 0U;
    *delay_ms_out = 0U;

    if (xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE) return false;
    const uint32_t generation = s_lifecycle_generation;
    const bool pending =
        s_lifecycle_stable_state == WEB_LIFECYCLE_STATE_STA_CONNECTED &&
        !s_lifecycle_transition_active &&
        !s_lifecycle_quiesce_pending &&
        !s_lifecycle_stop_requested &&
        web_mdns_retry_policy_pending(&s_mdns_retry_policy, generation);
    if (pending) {
        *generation_out = generation;
        *delay_ms_out = web_mdns_retry_policy_next_delay_ms(
            &s_mdns_retry_policy,
            generation,
            web_server_lifecycle_now_ms()
        );
    }
    (void)xSemaphoreGive(s_lifecycle_lock);
    return pending;
}

static void web_server_lifecycle_retry_mdns(uint32_t generation)
{
    if (s_lifecycle_lock == NULL || xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE) return;
    const bool due =
        s_lifecycle_generation == generation &&
        s_lifecycle_stable_state == WEB_LIFECYCLE_STATE_STA_CONNECTED &&
        !s_lifecycle_transition_active &&
        !s_lifecycle_quiesce_pending &&
        !s_lifecycle_stop_requested &&
        web_mdns_retry_policy_due(
            &s_mdns_retry_policy,
            generation,
            web_server_lifecycle_now_ms()
        );
    (void)xSemaphoreGive(s_lifecycle_lock);
    if (!due) return;

    if (
        s_https_server == NULL ||
        web_lifecycle_effective_network_state(
            WEB_LIFECYCLE_STATE_STA_CONNECTED,
            wifi_manager_is_radio_state_uncertain()
        ) != WEB_LIFECYCLE_STATE_STA_CONNECTED
    ) {
        if (xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) == pdTRUE) {
            if (s_lifecycle_generation == generation) {
                web_mdns_retry_policy_cancel(&s_mdns_retry_policy, generation);
            }
            (void)xSemaphoreGive(s_lifecycle_lock);
        }
        return;
    }

    const esp_err_t result = wifi_manager_mdns_is_ready()
        ? ESP_OK
        : wifi_manager_mdns_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "mDNS retry failed: %s", esp_err_to_name(result));
    }
    web_server_mdns_retry_record_result(generation, result);
}

void web_server_mdns_runtime_status(
    bool *ready_out,
    uint32_t *retry_count_out,
    uint32_t *next_retry_delay_ms_out
)
{
    if (ready_out != NULL) *ready_out = wifi_manager_mdns_is_ready();
    if (retry_count_out != NULL) *retry_count_out = 0U;
    if (next_retry_delay_ms_out != NULL) *next_retry_delay_ms_out = 0U;
    if (s_lifecycle_lock == NULL || xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY) != pdTRUE) return;

    if (s_mdns_retry_policy.generation == s_lifecycle_generation) {
        if (retry_count_out != NULL) *retry_count_out = s_mdns_retry_policy.retry_count;
        if (next_retry_delay_ms_out != NULL) {
            *next_retry_delay_ms_out = web_mdns_retry_policy_next_delay_ms(
                &s_mdns_retry_policy,
                s_lifecycle_generation,
                web_server_lifecycle_now_ms()
            );
        }
    }
    (void)xSemaphoreGive(s_lifecycle_lock);
}

static esp_err_t web_server_lifecycle_apply_state(
    web_lifecycle_state_t desired_state,
    uint32_t generation
)
{
    web_lifecycle_action_t actions[WEB_LIFECYCLE_ACTIONS_MAX];
    const size_t action_count = web_lifecycle_state_actions(
        desired_state,
        actions,
        sizeof(actions) / sizeof(actions[0])
    );
    if (
        action_count == 0U ||
        action_count > sizeof(actions) / sizeof(actions[0])
    ) {
        ESP_LOGW(
            TAG,
            "Invalid web lifecycle state=%d action_count=%u",
            (int)desired_state,
            (unsigned)action_count
        );
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t first_error = ESP_OK;
    bool http_stop_failed = false;
    bool https_start_failed = false;
    for (size_t i = 0U; i < action_count; ++i) {
        switch (actions[i]) {
        case WEB_LIFECYCLE_ACTION_MDNS_START: {
            if (desired_state == WEB_LIFECYCLE_STATE_STA_CONNECTED && https_start_failed) {
                wifi_manager_mdns_stop();
                ESP_LOGW(TAG, "Skipping mDNS because HTTPS did not start");
                break;
            }
            const esp_err_t mdns_err = wifi_manager_mdns_init();
            web_server_mdns_retry_record_result(generation, mdns_err);
            if (mdns_err != ESP_OK) {
                ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(mdns_err));
                if (first_error == ESP_OK) first_error = mdns_err;
            }
            if (wifi_manager_mdns_is_ready()) {
                ESP_LOGI(TAG, "Web UI: %s", APP_MDNS_WEB_URL);
            }
            break;
        }

        case WEB_LIFECYCLE_ACTION_MDNS_STOP:
            wifi_manager_mdns_stop();
            break;

        case WEB_LIFECYCLE_ACTION_HTTP_START: {
            const esp_err_t err = web_server_http_start_internal();
            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "HTTP bootstrap start for setup AP failed: %s",
                    esp_err_to_name(err)
                );
                if (first_error == ESP_OK) first_error = err;
            }
            break;
        }

        case WEB_LIFECYCLE_ACTION_HTTP_STOP:
            if (s_http_server != NULL) {
                web_server_log_tls_heap("before_http_stop");
                const esp_err_t err = web_server_http_stop_internal();
                if (err != ESP_OK) {
                    ESP_LOGE(
                        TAG,
                        "HTTP bootstrap stop for web lifecycle failed: %s",
                        esp_err_to_name(err)
                    );
                    http_stop_failed = true;
                    if (first_error == ESP_OK) first_error = err;
                }
                if (!http_stop_failed) {
                    web_server_log_tls_heap("after_http_stop");
                }
            }
            break;

        case WEB_LIFECYCLE_ACTION_HTTPS_START: {
            if (desired_state == WEB_LIFECYCLE_STATE_STA_CONNECTED && http_stop_failed) {
                ESP_LOGW(TAG, "Skipping HTTPS start because HTTP bootstrap did not stop");
                https_start_failed = true;
                break;
            }
            const esp_err_t err = web_server_https_start_internal();
            if (err != ESP_OK) {
                https_start_failed = true;
                if (err != ESP_ERR_NOT_FOUND) {
                    ESP_LOGE(TAG, "HTTPS start for STA failed: %s", esp_err_to_name(err));
                    if (first_error == ESP_OK) first_error = err;
                }
            }
            break;
        }

        case WEB_LIFECYCLE_ACTION_HTTPS_STOP: {
            const esp_err_t err = web_server_https_stop_internal();
            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "HTTPS stop for web lifecycle failed: %s",
                    esp_err_to_name(err)
                );
                if (first_error == ESP_OK) first_error = err;
            }
            break;
        }

        case WEB_LIFECYCLE_ACTION_EXIT:
            s_lifecycle_task = NULL;
            vTaskDeleteWithCaps(NULL);
            return first_error;

        default:
            ESP_LOGW(TAG, "Unknown web lifecycle action=%d", (int)actions[i]);
            if (first_error == ESP_OK) first_error = ESP_ERR_INVALID_STATE;
            break;
        }
    }

    return first_error;
}


static void web_server_lifecycle_task(void *argument)
{
    (void)argument;

    for (;;) {
        web_lifecycle_command_t command;
        uint32_t retry_generation = 0U;
        uint32_t retry_delay_ms = 0U;
        const bool retry_pending = web_server_mdns_retry_wait(
            &retry_generation,
            &retry_delay_ms
        );
        TickType_t receive_timeout = portMAX_DELAY;
        if (retry_pending) {
            receive_timeout = pdMS_TO_TICKS(retry_delay_ms);
            if (retry_delay_ms > 0U && receive_timeout == 0U) receive_timeout = 1U;
        }

        if (xQueueReceive(s_lifecycle_queue, &command, receive_timeout) == pdTRUE) {
            const esp_err_t apply_err = web_server_lifecycle_apply_state(
                command.state,
                command.generation
            );
            if (command.state == WEB_LIFECYCLE_STATE_QUIESCED) {
                web_server_lifecycle_ack_quiesce(apply_err);
            }
        }
        else if (retry_pending) {
            web_server_lifecycle_retry_mdns(retry_generation);
        }
    }
}


static esp_err_t web_server_lifecycle_start(void)
{
    if (s_lifecycle_queue != NULL || s_lifecycle_task != NULL) {
        return s_lifecycle_queue != NULL && s_lifecycle_task != NULL
            ? ESP_OK
            : ESP_ERR_INVALID_STATE;
    }

    s_lifecycle_stop_requested = false;
    s_lifecycle_quiesce_pending = false;
    s_lifecycle_transition_active = false;
    s_lifecycle_deferred_state_valid = false;
    s_lifecycle_stable_state =
        wifi_manager_is_setup_mode()
            ? WEB_LIFECYCLE_STATE_SETUP_AP
            : WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
    s_lifecycle_rollback_state = s_lifecycle_stable_state;
    s_lifecycle_quiesce_ack = NULL;
    s_lifecycle_quiesce_callers = 0U;
    s_lifecycle_quiesce_result = ESP_FAIL;

    s_lifecycle_generation = 0U;
    web_mdns_retry_policy_cancel(&s_mdns_retry_policy, s_lifecycle_generation);
    s_lifecycle_queue = xQueueCreate(1U, sizeof(web_lifecycle_command_t));
    s_lifecycle_lock = xSemaphoreCreateMutex();
    if (s_lifecycle_queue == NULL || s_lifecycle_lock == NULL) {
        if (s_lifecycle_queue != NULL) vQueueDelete(s_lifecycle_queue);
        if (s_lifecycle_lock != NULL) vSemaphoreDelete(s_lifecycle_lock);
        s_lifecycle_queue = NULL;
        s_lifecycle_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        web_server_lifecycle_task,
        "web_lifecycle",
        APP_TASK_STACK_WEB_LIFECYCLE,
        NULL,
        APP_TASK_PRIORITY_WEB_LIFECYCLE,
        &s_lifecycle_task,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (result != pdPASS) {
        vQueueDelete(s_lifecycle_queue);
        vSemaphoreDelete(s_lifecycle_lock);
        s_lifecycle_queue = NULL;
        s_lifecycle_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "WEB_LIFECYCLE initialized core=%d stack=%u stack_caps=spiram|8bit",
        APP_TASK_CORE_SERVICES,
        APP_TASK_STACK_WEB_LIFECYCLE
    );
    return ESP_OK;
}


static void web_server_lifecycle_stop(void)
{
    if (s_lifecycle_lock == NULL) return;

    SemaphoreHandle_t lock = s_lifecycle_lock;
    (void)xSemaphoreTake(lock, portMAX_DELAY);
    QueueHandle_t queue = s_lifecycle_queue;
    if (queue != NULL && s_lifecycle_task != NULL) {
        s_lifecycle_stop_requested = true;
        if (web_server_lifecycle_queue_state_locked(WEB_LIFECYCLE_STATE_STOPPED) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to queue web lifecycle stop");
        }
    }
    else {
        s_lifecycle_stop_requested = true;
    }
    (void)xSemaphoreGive(lock);

    while (s_lifecycle_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    for (;;) {
        (void)xSemaphoreTake(lock, portMAX_DELAY);
        const bool quiesce_call_in_flight =
            s_lifecycle_quiesce_callers != 0U;
        (void)xSemaphoreGive(lock);
        if (!quiesce_call_in_flight) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    (void)xSemaphoreTake(lock, portMAX_DELAY);
    s_lifecycle_queue = NULL;
    s_lifecycle_quiesce_ack = NULL;
    s_lifecycle_quiesce_pending = false;
    s_lifecycle_transition_active = false;
    s_lifecycle_deferred_state_valid = false;
    s_lifecycle_stable_state = WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
    s_lifecycle_rollback_state = WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
    (void)xSemaphoreGive(lock);

    if (queue != NULL) vQueueDelete(queue);
    vSemaphoreDelete(lock);
    s_lifecycle_lock = NULL;
}


static void web_server_network_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    (void)event_data;

    web_lifecycle_state_t event_state;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        event_state = WEB_LIFECYCLE_STATE_STA_CONNECTED;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        event_state = WEB_LIFECYCLE_STATE_SETUP_AP;
    }
    else if (
        event_base == WIFI_EVENT &&
        (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)
    ) {
        event_state = WEB_LIFECYCLE_STATE_STA_DISCONNECTED;
    }
    else {
        return;
    }

    const esp_err_t err =
        web_server_lifecycle_set_network_event_state(event_state);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "WEB_LIFECYCLE state=%d was not queued: %s",
            (int)event_state,
            esp_err_to_name(err)
        );
    }
}

static void web_server_https_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)context;
    if (event_base != ESP_HTTPS_SERVER_EVENT) return;

    if (event_id == HTTPS_SERVER_EVENT_START) {
        web_server_log_tls_heap("event_start");
        return;
    }

    if (event_id == HTTPS_SERVER_EVENT_ON_CONNECTED) {
        uint32_t clients;
        portENTER_CRITICAL(&s_https_stats_lock);
        if (s_https_clients < UINT32_MAX) ++s_https_clients;
        if (s_https_clients > s_https_max_clients) s_https_max_clients = s_https_clients;
        clients = s_https_clients;
        portEXIT_CRITICAL(&s_https_stats_lock);
        ESP_LOGI(
            TAG,
            "WEB_TIMING stage=tls_connected ts_us=%lld clients=%u",
            (long long)esp_timer_get_time(),
            (unsigned)clients
        );
        web_server_log_tls_heap("client_connected");
        runtime_observability_request_snapshot("tls-connected");
        return;
    }

    if (event_id == HTTPS_SERVER_EVENT_DISCONNECTED) {
        portENTER_CRITICAL(&s_https_stats_lock);
        if (s_https_clients > 0U) --s_https_clients;
        portEXIT_CRITICAL(&s_https_stats_lock);
        web_server_log_tls_heap("client_disconnected");
        runtime_observability_request_snapshot("tls-disconnected");
        return;
    }

    if (event_id == HTTPS_SERVER_EVENT_ERROR && event_data != NULL) {
        const esp_https_server_last_error_t *last_error =
            (const esp_https_server_last_error_t *)event_data;

        const bool peer_fatal_alert =
            (unsigned)last_error->esp_tls_error_code == WEB_TLS_FATAL_ALERT_CODE;
        uint32_t clients;
        uint32_t fatal_alerts;
        portENTER_CRITICAL(&s_https_stats_lock);
        if (s_https_handshake_failures < UINT32_MAX) ++s_https_handshake_failures;
        s_https_last_error_esp_err = (int32_t)last_error->last_error;
        if (peer_fatal_alert && s_peer_fatal_alerts < UINT32_MAX) ++s_peer_fatal_alerts;
        clients = s_https_clients;
        fatal_alerts = s_peer_fatal_alerts;
        portEXIT_CRITICAL(&s_https_stats_lock);

        if (peer_fatal_alert) {
            ESP_LOGW(
                TAG,
                "TLS peer fatal alert (-0x7780): browser rejected/aborted handshake; verify GeoLocator Root CA trust on this client (alerts=%u clients=%u)",
                (unsigned)fatal_alerts,
                (unsigned)clients
            );
            return;
        }

        ESP_LOGE(
            TAG,
            "HTTPS event error: last=%s tls=%d flags=%d clients=%u",
            esp_err_to_name(last_error->last_error),
            last_error->esp_tls_error_code,
            last_error->esp_tls_flags,
            (unsigned)clients
        );
    }
}

static esp_err_t web_server_event_handlers_register(void)
{
    if (s_event_handlers_registered) return ESP_OK;

    esp_err_t err = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        web_server_network_event_handler,
        NULL
    );
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        web_server_network_event_handler,
        NULL
    );
    if (err != ESP_OK) goto fail_ip;

    err = esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_STOP,
        web_server_network_event_handler,
        NULL
    );
    if (err != ESP_OK) goto fail_disconnect;

    err = esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_AP_START,
        web_server_network_event_handler,
        NULL
    );
    if (err != ESP_OK) goto fail_sta_stop;

    err = esp_event_handler_register(
        ESP_HTTPS_SERVER_EVENT,
        ESP_EVENT_ANY_ID,
        web_server_https_event_handler,
        NULL
    );
    if (err != ESP_OK) goto fail_ap_start;

    s_event_handlers_registered = true;
    return ESP_OK;

fail_ap_start:
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, web_server_network_event_handler);
fail_sta_stop:
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_STOP, web_server_network_event_handler);
fail_disconnect:
    (void)esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, web_server_network_event_handler);
fail_ip:
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, web_server_network_event_handler);
    return err;
}

static void web_server_event_handlers_unregister(void)
{
    if (!s_event_handlers_registered) return;

    (void)esp_event_handler_unregister(
        ESP_HTTPS_SERVER_EVENT,
        ESP_EVENT_ANY_ID,
        web_server_https_event_handler
    );
    (void)esp_event_handler_unregister(
        WIFI_EVENT,
        WIFI_EVENT_AP_START,
        web_server_network_event_handler
    );
    (void)esp_event_handler_unregister(
        WIFI_EVENT,
        WIFI_EVENT_STA_STOP,
        web_server_network_event_handler
    );
    (void)esp_event_handler_unregister(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        web_server_network_event_handler
    );
    (void)esp_event_handler_unregister(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        web_server_network_event_handler
    );
    s_event_handlers_registered = false;
}

esp_err_t web_server_start(void)
{
    web_cjson_install_psram_hooks();

    if (s_event_handlers_registered) {
        ESP_LOGW(TAG, "Web server lifecycle already running");
        return ESP_OK;
    }

    web_server_status_cache_start();

    esp_err_t err = web_server_lifecycle_start();
    if (err != ESP_OK) {
        web_server_status_cache_stop();
        return err;
    }

    err = wifi_manager_set_radio_stop_lifecycle_callbacks(
        web_server_lifecycle_quiesce,
        web_server_lifecycle_radio_stop_complete,
        NULL
    );
    if (err != ESP_OK) {
        web_server_lifecycle_stop();
        web_server_status_cache_stop();
        return err;
    }

    err = wifi_manager_set_mode_transition_complete_callback(
        web_server_mode_transition_complete,
        NULL
    );
    if (err != ESP_OK) {
        web_server_lifecycle_unregister_wifi_hook();
        web_server_lifecycle_stop();
        web_server_status_cache_stop();
        return err;
    }

    /*
     * Bootstrap HTTP before registering the network callbacks. This keeps a
     * pending AP_START event from racing the initial main-task start; every
     * later AP/STA transition is serialized by the lifecycle worker.
     */
    if (wifi_manager_is_setup_mode()) {
        err = web_server_http_start_internal();
        if (err != ESP_OK) {
            web_server_unregister_mode_transition_callback();
            web_server_lifecycle_unregister_wifi_hook();
            web_server_lifecycle_stop();
            web_server_status_cache_stop();
            return err;
        }
    }

    err = web_server_event_handlers_register();
    if (err != ESP_OK) {
        web_server_unregister_mode_transition_callback();
        web_server_lifecycle_unregister_wifi_hook();
        web_server_lifecycle_stop();
        web_server_status_cache_stop();
        return err;
    }

    if (wifi_manager_is_setup_mode()) {
        err = web_server_lifecycle_set_desired_state(
            WEB_LIFECYCLE_STATE_SETUP_AP
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Setup AP lifecycle request failed: %s", esp_err_to_name(err));
            web_server_event_handlers_unregister();
            web_server_unregister_mode_transition_callback();
            web_server_lifecycle_unregister_wifi_hook();
            web_server_lifecycle_stop();
            web_server_status_cache_stop();
            return err;
        }
        return ESP_OK;
    }

    char sta_ip[16];
    if (wifi_manager_sta_ip_string(sta_ip, sizeof(sta_ip)) == ESP_OK) {
        err = web_server_lifecycle_set_desired_state(
            WEB_LIFECYCLE_STATE_STA_CONNECTED
        );
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTPS startup request failed: %s", esp_err_to_name(err));
            web_server_event_handlers_unregister();
            web_server_unregister_mode_transition_callback();
            web_server_lifecycle_unregister_wifi_hook();
            web_server_lifecycle_stop();
            web_server_status_cache_stop();
            return err;
        }
    }
    else {
        ESP_LOGI(TAG, "STA connecting: waiting for IP before starting HTTPS");
    }

    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    web_server_event_handlers_unregister();
    web_server_unregister_mode_transition_callback();
    web_server_lifecycle_unregister_wifi_hook();
    web_server_lifecycle_stop();
    web_server_status_cache_stop();

    esp_err_t first_error = ESP_OK;
    esp_err_t err = web_server_https_stop_internal();
    if (err != ESP_OK) first_error = err;

    err = web_server_http_stop_internal();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    return first_error;
}
