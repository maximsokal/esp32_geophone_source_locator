#include "web_server_routes.h"

#include <stddef.h>

#include "web_api_status.h"
#include "web_page.h"
#include "web_seismic.h"
#include "web_static_assets.h"
#include "web_wifi_setup.h"
#include "wifi_manager.h"


static esp_err_t web_server_index_handler(
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


static esp_err_t web_server_register_core_routes(
    httpd_handle_t server
)
{
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_server_index_handler,
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


esp_err_t web_server_routes_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err =
        web_server_register_core_routes(
            server
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        web_wifi_setup_register(
            server
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
