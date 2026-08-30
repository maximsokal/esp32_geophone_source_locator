#include "web_server_internal.h"

#include <stddef.h>

#include "app_config.h"
#include "wifi_manager.h"


static esp_err_t web_server_bootstrap_index_handler(
    httpd_req_t *request
)
{
    if (
        wifi_manager_is_setup_mode()
    ) {
        return
            web_wifi_setup_send_page(
                request
            );
    }

    static const char response[] =
        "GeoLocator secure web UI is available over HTTPS.";

    httpd_resp_set_status(
        request,
        "302 Found"
    );

    httpd_resp_set_hdr(
        request,
        "Location",
        APP_MDNS_WEB_URL
    );

    httpd_resp_set_type(
        request,
        "text/plain; charset=utf-8"
    );

    return
        httpd_resp_send(
            request,
            response,
            HTTPD_RESP_USE_STRLEN
        );
}


static esp_err_t web_server_secure_index_handler(
    httpd_req_t *request
)
{
    return
        web_page_send_index(
            request
        );
}


static esp_err_t web_server_health_handler(
    httpd_req_t *request
)
{
    static const char response[] =
        "OK";

    httpd_resp_set_type(
        request,
        "text/plain"
    );

    return
        httpd_resp_send(
            request,
            response,
            HTTPD_RESP_USE_STRLEN
        );
}


static esp_err_t web_server_register_root_and_health(
    httpd_handle_t server,
    esp_err_t (*root_handler)(httpd_req_t *request)
)
{
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };

    esp_err_t err =
        httpd_register_uri_handler(
            server,
            &index_uri
        );

    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = web_server_health_handler,
        .user_ctx = NULL,
    };

    return
        httpd_register_uri_handler(
            server,
            &health_uri
        );
}


esp_err_t web_server_bootstrap_routes_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        web_server_register_root_and_health(
            server,
            web_server_bootstrap_index_handler
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        web_wifi_setup_register(
            server
        );
}


esp_err_t web_server_routes_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        web_server_register_root_and_health(
            server,
            web_server_secure_index_handler
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        web_static_assets_register(
            server
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        web_api_status_register(
            server
        );

    if (err != ESP_OK) {
        return err;
    }

    return
        web_seismic_register(
            server
        );
}
