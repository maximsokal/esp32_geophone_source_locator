#include "web_server.h"

#include <stdbool.h>
#include <stddef.h>

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
#include "web_server_internal.h"
#include "web_route_capacity.h"

#define WEB_TLS_FATAL_ALERT_CODE 0x7780
#define WEB_TLS_HANDSHAKE_TIMEOUT_MS 5000U
#define WEB_HTTPS_TASK_STACK_BYTES   8192U
#define WEB_HTTPS_MAX_CLIENTS           2U

typedef enum {
    WEB_LIFECYCLE_COMMAND_STOP = 0,
    WEB_LIFECYCLE_COMMAND_STA_GOT_IP,
    WEB_LIFECYCLE_COMMAND_AP_START,
    WEB_LIFECYCLE_COMMAND_STA_LOST,
} web_lifecycle_command_t;

static const char *TAG = "WEB_SERVER";

static httpd_handle_t s_http_server = NULL;
static httpd_handle_t s_https_server = NULL;
static QueueHandle_t s_lifecycle_queue = NULL;
static SemaphoreHandle_t s_lifecycle_lock = NULL;
static TaskHandle_t s_lifecycle_task = NULL;
static bool s_event_handlers_registered = false;
static bool s_measurement_hooks_registered = false;
static bool s_object_observer_registered = false;
static unsigned s_https_clients = 0;
static unsigned s_peer_fatal_alerts = 0;

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

static void web_server_log_tls_heap(const char *stage)
{
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(
        TAG,
        "TLS_HEAP %s clients=%u free=%u largest=%u peer_alerts=%u",
        stage,
        s_https_clients,
        (unsigned)free_internal,
        (unsigned)largest_internal,
        s_peer_fatal_alerts
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


static esp_err_t web_server_lifecycle_request(
    web_lifecycle_command_t command
)
{
    if (
        s_lifecycle_lock == NULL ||
        xSemaphoreTake(s_lifecycle_lock, 0) != pdTRUE
    ) {
        return ESP_ERR_TIMEOUT;
    }

    const bool ready =
        s_lifecycle_queue != NULL &&
        s_lifecycle_task != NULL;
    const BaseType_t result = ready
        ? xQueueOverwrite(s_lifecycle_queue, &command)
        : pdFALSE;

    (void)xSemaphoreGive(s_lifecycle_lock);

    if (!ready) return ESP_ERR_INVALID_STATE;
    return result == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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

    s_peer_fatal_alerts = 0;
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
        "Starting HTTPS server on port %u (max clients=%u, stack=%u, LRU=on, session tickets=on, handshake timeout=%u ms)",
        (unsigned)config.port_secure,
        (unsigned)config.httpd.max_open_sockets,
        (unsigned)config.httpd.stack_size,
        (unsigned)config.tls_handshake_timeout_ms
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
        s_https_clients = 0;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping HTTPS server");
    esp_err_t err = httpd_ssl_stop(s_https_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTPS server: %s", esp_err_to_name(err));
        return err;
    }

    s_https_server = NULL;
    s_https_clients = 0;
    s_peer_fatal_alerts = 0;
    ESP_LOGI(TAG, "HTTPS server stopped");
    return ESP_OK;
}

static esp_err_t web_server_http_start_internal(void)
{
    if (s_http_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = APP_WEB_HTTP_PORT;
    config.max_uri_handlers = WEB_HTTP_URI_HANDLERS_CAPACITY;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "URI handlers required=%u capacity=%u",
             (unsigned)WEB_HTTP_URI_HANDLERS_REQUIRED, (unsigned)config.max_uri_handlers);
    ESP_LOGI(TAG, "Starting HTTP bootstrap server on port %u", (unsigned)config.server_port);
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

static void web_server_lifecycle_handle_sta_got_ip(void)
{
    const esp_err_t mdns_err = wifi_manager_mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: %s", esp_err_to_name(mdns_err));
    }

    if (wifi_manager_mdns_is_ready()) {
        ESP_LOGI(TAG, "Web UI: %s", APP_MDNS_WEB_URL);
    }

    if (s_http_server != NULL) {
        web_server_log_tls_heap("before_http_stop");
        const esp_err_t http_stop_err = web_server_http_stop_internal();
        if (http_stop_err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "HTTP bootstrap stop after STA IP failed: %s",
                esp_err_to_name(http_stop_err)
            );
            return;
        }
        web_server_log_tls_heap("after_http_stop");
    }

    const esp_err_t https_start_err = web_server_https_start_internal();
    if (https_start_err != ESP_OK && https_start_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(
            TAG,
            "HTTPS start after STA IP failed: %s",
            esp_err_to_name(https_start_err)
        );
    }
}


static void web_server_lifecycle_handle_ap_start(void)
{
    const esp_err_t https_stop_err = web_server_https_stop_internal();
    if (https_stop_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "HTTPS stop before setup AP failed: %s",
            esp_err_to_name(https_stop_err)
        );
    }

    const esp_err_t http_start_err = web_server_http_start_internal();
    if (http_start_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "HTTP bootstrap start for setup AP failed: %s",
            esp_err_to_name(http_start_err)
        );
    }
}


static void web_server_lifecycle_handle_sta_lost(void)
{
    const esp_err_t err = web_server_https_stop_internal();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS stop after STA loss failed: %s", esp_err_to_name(err));
    }
}


static void web_server_lifecycle_task(void *argument)
{
    (void)argument;

    for (;;) {
        web_lifecycle_command_t command;
        if (xQueueReceive(s_lifecycle_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (command) {
        case WEB_LIFECYCLE_COMMAND_STOP:
            (void)web_server_https_stop_internal();
            (void)web_server_http_stop_internal();
            s_lifecycle_task = NULL;
            vTaskDeleteWithCaps(NULL);
            return;

        case WEB_LIFECYCLE_COMMAND_STA_GOT_IP:
            web_server_lifecycle_handle_sta_got_ip();
            break;

        case WEB_LIFECYCLE_COMMAND_AP_START:
            web_server_lifecycle_handle_ap_start();
            break;

        case WEB_LIFECYCLE_COMMAND_STA_LOST:
            web_server_lifecycle_handle_sta_lost();
            break;

        default:
            ESP_LOGW(TAG, "Unknown web lifecycle command=%d", (int)command);
            break;
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

    (void)xSemaphoreTake(s_lifecycle_lock, portMAX_DELAY);
    QueueHandle_t queue = s_lifecycle_queue;
    if (queue != NULL && s_lifecycle_task != NULL) {
        const web_lifecycle_command_t command = WEB_LIFECYCLE_COMMAND_STOP;
        if (xQueueOverwrite(queue, &command) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to queue web lifecycle stop");
        }
        while (s_lifecycle_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_lifecycle_queue = NULL;
    (void)xSemaphoreGive(s_lifecycle_lock);

    if (queue != NULL) vQueueDelete(queue);
    vSemaphoreDelete(s_lifecycle_lock);
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

    web_lifecycle_command_t command;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        command = WEB_LIFECYCLE_COMMAND_STA_GOT_IP;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        command = WEB_LIFECYCLE_COMMAND_AP_START;
    }
    else if (
        event_base == WIFI_EVENT &&
        (event_id == WIFI_EVENT_STA_DISCONNECTED || event_id == WIFI_EVENT_STA_STOP)
    ) {
        command = WEB_LIFECYCLE_COMMAND_STA_LOST;
    }
    else {
        return;
    }

    const esp_err_t err = web_server_lifecycle_request(command);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "WEB_LIFECYCLE event command=%d was not queued: %s",
            (int)command,
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
        ++s_https_clients;
        ESP_LOGI(
            TAG,
            "WEB_TIMING stage=tls_connected ts_us=%lld clients=%u",
            (long long)esp_timer_get_time(),
            s_https_clients
        );
        web_server_log_tls_heap("client_connected");
        runtime_observability_request_snapshot("tls-connected");
        return;
    }

    if (event_id == HTTPS_SERVER_EVENT_DISCONNECTED) {
        if (s_https_clients > 0) --s_https_clients;
        web_server_log_tls_heap("client_disconnected");
        runtime_observability_request_snapshot("tls-disconnected");
        return;
    }

    if (event_id == HTTPS_SERVER_EVENT_ERROR && event_data != NULL) {
        const esp_https_server_last_error_t *last_error =
            (const esp_https_server_last_error_t *)event_data;

        if ((unsigned)last_error->esp_tls_error_code == WEB_TLS_FATAL_ALERT_CODE) {
            ++s_peer_fatal_alerts;
            ESP_LOGW(
                TAG,
                "TLS peer fatal alert (-0x7780): browser rejected/aborted handshake; verify GeoLocator Root CA trust on this client (alerts=%u clients=%u)",
                s_peer_fatal_alerts,
                s_https_clients
            );
            return;
        }

        ESP_LOGE(
            TAG,
            "HTTPS event error: last=%s tls=%d flags=%d clients=%u",
            esp_err_to_name(last_error->last_error),
            last_error->esp_tls_error_code,
            last_error->esp_tls_flags,
            s_https_clients
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

    /*
     * Bootstrap HTTP before registering the network callbacks. This keeps a
     * pending AP_START event from racing the initial main-task start; every
     * later AP/STA transition is serialized by the lifecycle worker.
     */
    if (wifi_manager_is_setup_mode()) {
        err = web_server_http_start_internal();
        if (err != ESP_OK) {
            web_server_lifecycle_stop();
            web_server_status_cache_stop();
            return err;
        }
    }

    err = web_server_event_handlers_register();
    if (err != ESP_OK) {
        web_server_lifecycle_stop();
        web_server_status_cache_stop();
        return err;
    }

    if (wifi_manager_is_setup_mode()) {
        return ESP_OK;
    }

    char sta_ip[16];
    if (wifi_manager_sta_ip_string(sta_ip, sizeof(sta_ip)) == ESP_OK) {
        err = web_server_lifecycle_request(WEB_LIFECYCLE_COMMAND_STA_GOT_IP);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTPS startup request failed: %s", esp_err_to_name(err));
            web_server_event_handlers_unregister();
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
    web_server_lifecycle_stop();
    web_server_status_cache_stop();

    esp_err_t first_error = ESP_OK;
    esp_err_t err = web_server_https_stop_internal();
    if (err != ESP_OK) first_error = err;

    err = web_server_http_stop_internal();
    if (err != ESP_OK && first_error == ESP_OK) first_error = err;
    return first_error;
}
