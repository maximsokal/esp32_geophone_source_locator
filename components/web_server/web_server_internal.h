#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "device_status.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "web_route_capacity.h"

void web_server_route_tracking_begin(size_t capacity);
esp_err_t web_server_route_tracking_finish(const web_route_identity_t *catalog, size_t count);
esp_err_t web_server_route_tracking_register(const char *uri, int method);

static inline esp_err_t web_server_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *route
)
{
    const esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE("WEB_ROUTES", "Failed to register method=%d uri=%s: %s",
                 route != NULL ? (int)route->method : -1,
                 route != NULL && route->uri != NULL ? route->uri : "(null)",
                 esp_err_to_name(err));
    }
    if (err != ESP_OK) return err;
    return web_server_route_tracking_register(route->uri, (int)route->method);
}

esp_err_t web_server_unregister_uri_handler(
    httpd_handle_t server,
    const char *uri,
    int method
);

#define WEB_JSON_WRITER_BUFFER_SIZE  768

typedef struct
{
    httpd_req_t *request;
    char buffer[WEB_JSON_WRITER_BUFFER_SIZE];
    size_t length;
    esp_err_t status;
} web_json_writer_t;

typedef struct
{
    const char *uri;
    const char *sd_suffix;
    const char *content_type;
    const unsigned char *start;
    const unsigned char *end;
} web_webui_chunk_asset_t;

typedef enum
{
    WEB_STATIC_ASSET_INDEX_HTML = 0,
    WEB_STATIC_ASSET_DASHBOARD_CSS,
    WEB_STATIC_ASSET_DASHBOARD_JS,
    WEB_STATIC_ASSET_SEISMIC_HTML,
    WEB_STATIC_ASSET_SEISMIC_CSS,
    WEB_STATIC_ASSET_SEISMIC_JS,
    WEB_STATIC_ASSET_COUNT
} web_static_asset_id_t;

esp_err_t web_server_bootstrap_routes_register(httpd_handle_t server);
esp_err_t web_server_routes_register(httpd_handle_t server);

esp_err_t web_api_status_register(httpd_handle_t server);
esp_err_t web_api_control_register(httpd_handle_t server);
esp_err_t web_scan_store_try_handle(httpd_req_t *request, bool *handled);

esp_err_t web_page_send_index(httpd_req_t *request);

esp_err_t web_webui_prepare(void);
esp_err_t web_webui_send_index(httpd_req_t *request);
esp_err_t web_webui_register(httpd_handle_t server);

/*
 * Runtime React delivery intentionally uses the embedded flash bundle.
 * The TFT and SD card share SPI2; avoiding FATFS/SDSPI access in HTTP
 * handlers prevents SD polling transactions from colliding with the
 * asynchronous esp_lcd transfer queue.
 */
esp_err_t web_webui_flash_send_index(httpd_req_t *request);
esp_err_t web_webui_flash_register(httpd_handle_t server);
/* Optional immutable copy of the embedded gzip bundle in PSRAM. */
esp_err_t web_webui_flash_prepare_cache(void);

esp_err_t web_seismic_register(httpd_handle_t server);
esp_err_t web_seismic_api_register(httpd_handle_t server);
esp_err_t web_seismic_page_send(httpd_req_t *request);

const char *web_static_asset_data(web_static_asset_id_t asset);

esp_err_t web_status_json_build(char **json_out);
esp_err_t web_status_json_build_extended(
    char **json_out,
    bool include_objects,
    const char *sessions_object_id
);
void web_status_json_free(char *json);

bool web_status_json_add_pods(cJSON *root, const device_status_snapshot_t *status);
esp_err_t web_status_json_objects_init(void);
void web_status_json_objects_shutdown(void);
esp_err_t web_status_json_objects_refresh(void);
esp_err_t web_status_json_objects_request_refresh(void);
bool web_status_json_add_objects(cJSON *root);
void web_status_json_objects_invalidate(void);
bool web_status_json_objects_cache_ready(void);
bool web_status_json_objects_cache_busy(void);
esp_err_t web_status_json_objects_start_guard(void *user_context);
void web_status_json_objects_release_start_reservation(void *user_context);
void web_status_json_objects_storage_changed(void *user_context);
void web_status_json_objects_set_active(const char *object_id);
bool web_status_json_add_recording(cJSON *root);
bool web_status_json_add_measurement(cJSON *root);
bool web_status_json_add_sessions(cJSON *root, const char *object_id);

void web_json_writer_init(web_json_writer_t *writer, httpd_req_t *request);
esp_err_t web_json_writer_write(web_json_writer_t *writer, const char *text);
esp_err_t web_json_writer_printf(web_json_writer_t *writer, const char *format, ...);
esp_err_t web_json_writer_finish(web_json_writer_t *writer);

esp_err_t web_wifi_setup_send_page(httpd_req_t *request);
esp_err_t web_wifi_setup_register(httpd_handle_t server);
esp_err_t web_wifi_setup_url_decode(const char *source, char *destination, size_t destination_size);
esp_err_t web_wifi_setup_form_parse(char *body, char *ssid, size_t ssid_size, char *password, size_t password_size);
esp_err_t web_wifi_setup_page_send_form(httpd_req_t *request);
esp_err_t web_wifi_setup_page_send_saved(httpd_req_t *request);
esp_err_t web_wifi_setup_page_open_handler(httpd_req_t *request);
esp_err_t web_wifi_setup_request_read_body(httpd_req_t *request, char *body, size_t body_size);

esp_err_t web_https_onboarding_bootstrap_register(httpd_handle_t server);
esp_err_t web_https_onboarding_secure_register(httpd_handle_t server);
esp_err_t web_https_onboarding_ui_bootstrap_register(httpd_handle_t server);
esp_err_t web_https_onboarding_ui_secure_register(httpd_handle_t server);
bool web_https_onboarding_ca_install_confirmed(void);
