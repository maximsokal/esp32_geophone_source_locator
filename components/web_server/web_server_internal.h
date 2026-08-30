#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "device_status.h"
#include "esp_err.h"
#include "esp_http_server.h"


#define WEB_JSON_WRITER_BUFFER_SIZE  768


typedef struct
{
    httpd_req_t *request;

    char buffer[
        WEB_JSON_WRITER_BUFFER_SIZE
    ];

    size_t length;
    esp_err_t status;

} web_json_writer_t;


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

esp_err_t web_page_send_index(httpd_req_t *request);

esp_err_t web_seismic_register(httpd_handle_t server);
esp_err_t web_seismic_api_register(httpd_handle_t server);
esp_err_t web_seismic_page_send(httpd_req_t *request);

const char *web_static_asset_data(web_static_asset_id_t asset);
esp_err_t web_static_assets_register(httpd_handle_t server);

esp_err_t web_status_json_build(char **json_out);
void web_status_json_free(char *json);

bool web_status_json_add_pods(
    cJSON *root,
    const device_status_snapshot_t *status
);

void web_json_writer_init(
    web_json_writer_t *writer,
    httpd_req_t *request
);

esp_err_t web_json_writer_write(
    web_json_writer_t *writer,
    const char *text
);

esp_err_t web_json_writer_printf(
    web_json_writer_t *writer,
    const char *format,
    ...
);

esp_err_t web_json_writer_finish(web_json_writer_t *writer);

esp_err_t web_wifi_setup_send_page(httpd_req_t *request);
esp_err_t web_wifi_setup_register(httpd_handle_t server);

esp_err_t web_wifi_setup_url_decode(
    const char *source,
    char *destination,
    size_t destination_size
);

esp_err_t web_wifi_setup_form_parse(
    char *body,
    char *ssid,
    size_t ssid_size,
    char *password,
    size_t password_size
);

esp_err_t web_wifi_setup_page_send_form(httpd_req_t *request);
esp_err_t web_wifi_setup_page_send_saved(httpd_req_t *request);
esp_err_t web_wifi_setup_page_open_handler(httpd_req_t *request);

esp_err_t web_wifi_setup_request_read_body(
    httpd_req_t *request,
    char *body,
    size_t body_size
);
