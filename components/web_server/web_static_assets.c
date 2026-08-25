#include "web_static_assets.h"

#include <stddef.h>


static esp_err_t web_static_send(
    httpd_req_t *request,
    web_static_asset_id_t asset,
    const char *content_type
)
{
    const char *data =
        web_static_asset_data(
            asset
        );

    if (
        request == NULL ||
        data == NULL ||
        content_type == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_type(
        request,
        content_type
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    return
        httpd_resp_send(
            request,
            data,
            HTTPD_RESP_USE_STRLEN
        );
}


static esp_err_t dashboard_css_handler(
    httpd_req_t *request
)
{
    return
        web_static_send(
            request,
            WEB_STATIC_ASSET_DASHBOARD_CSS,
            "text/css; charset=utf-8"
        );
}


static esp_err_t dashboard_js_handler(
    httpd_req_t *request
)
{
    return
        web_static_send(
            request,
            WEB_STATIC_ASSET_DASHBOARD_JS,
            "application/javascript; charset=utf-8"
        );
}


static esp_err_t seismic_css_handler(
    httpd_req_t *request
)
{
    return
        web_static_send(
            request,
            WEB_STATIC_ASSET_SEISMIC_CSS,
            "text/css; charset=utf-8"
        );
}


static esp_err_t seismic_js_handler(
    httpd_req_t *request
)
{
    return
        web_static_send(
            request,
            WEB_STATIC_ASSET_SEISMIC_JS,
            "application/javascript; charset=utf-8"
        );
}


esp_err_t web_static_assets_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t routes[] = {
        {
            .uri = "/assets/dashboard.css",
            .method = HTTP_GET,
            .handler = dashboard_css_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/assets/dashboard.js",
            .method = HTTP_GET,
            .handler = dashboard_js_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/assets/seismic.css",
            .method = HTTP_GET,
            .handler = seismic_css_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/assets/seismic.js",
            .method = HTTP_GET,
            .handler = seismic_js_handler,
            .user_ctx = NULL,
        },
    };

    for (
        size_t i = 0;
        i <
            sizeof(routes) /
            sizeof(routes[0]);
        ++i
    ) {
        esp_err_t err =
            httpd_register_uri_handler(
                server,
                &routes[i]
            );

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}
