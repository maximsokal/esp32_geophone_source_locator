#include "web_server_internal.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"


#define WEB_STATIC_CHUNK_BYTES       512U
#define WEB_STATIC_SEND_TIMEOUT_SEC  30


static const char *TAG =
    "WEB_STATIC";


static void web_static_configure_send_timeout(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return;
    }

    const int socket_fd =
        httpd_req_to_sockfd(
            request
        );

    if (socket_fd < 0) {
        ESP_LOGW(
            TAG,
            "Could not resolve response socket"
        );
        return;
    }

    const struct timeval timeout = {
        .tv_sec = WEB_STATIC_SEND_TIMEOUT_SEC,
        .tv_usec = 0,
    };

    if (
        setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &timeout,
            sizeof(timeout)
        ) != 0
    ) {
        ESP_LOGW(
            TAG,
            "Could not set %d s send timeout on socket %d: errno=%d",
            WEB_STATIC_SEND_TIMEOUT_SEC,
            socket_fd,
            errno
        );
    }
}


static esp_err_t web_static_send(
    httpd_req_t *request,
    web_static_asset_id_t asset,
    const char *content_type,
    const char *label
)
{
    const char *data =
        web_static_asset_data(
            asset
        );

    if (
        request == NULL ||
        data == NULL ||
        content_type == NULL ||
        label == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    web_static_configure_send_timeout(
        request
    );

    httpd_resp_set_type(
        request,
        content_type
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    const size_t length =
        strlen(data);

    size_t offset =
        0;

    while (offset < length) {
        const size_t remaining =
            length - offset;

        const size_t chunk_length =
            remaining > WEB_STATIC_CHUNK_BYTES
                ? WEB_STATIC_CHUNK_BYTES
                : remaining;

        const esp_err_t err =
            httpd_resp_send_chunk(
                request,
                data + offset,
                chunk_length
            );

        if (err != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to send %s chunk at offset=%u length=%u: %s",
                label,
                (unsigned)offset,
                (unsigned)chunk_length,
                esp_err_to_name(err)
            );

            return err;
        }

        offset +=
            chunk_length;
    }

    const esp_err_t err =
        httpd_resp_send_chunk(
            request,
            NULL,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to finalize %s response: %s",
            label,
            esp_err_to_name(err)
        );
        return err;
    }

    ESP_LOGI(
        TAG,
        "%s response completed: %u bytes",
        label,
        (unsigned)length
    );

    return ESP_OK;
}


static esp_err_t dashboard_css_handler(
    httpd_req_t *request
)
{
    return
        web_static_send(
            request,
            WEB_STATIC_ASSET_DASHBOARD_CSS,
            "text/css; charset=utf-8",
            "dashboard.css"
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
            "application/javascript; charset=utf-8",
            "dashboard.js"
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
            "text/css; charset=utf-8",
            "seismic.css"
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
            "application/javascript; charset=utf-8",
            "seismic.js"
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
