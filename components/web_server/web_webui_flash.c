#include "web_server_internal.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"


#define WEB_WEBUI_FLASH_CHUNK_BYTES  1024U


static const char *TAG =
    "WEB_WEBUI_FLASH";


#if WEB_SERVER_WEBUI_SEED_EMBEDDED
extern const unsigned char webui_index_start[]
    asm("_binary_generated_webui_index_html_start");
extern const unsigned char webui_index_end[]
    asm("_binary_generated_webui_index_html_end");

extern const unsigned char webui_js_gz_start[]
    asm("_binary_generated_webui_assets_app_js_gz_start");
extern const unsigned char webui_js_gz_end[]
    asm("_binary_generated_webui_assets_app_js_gz_end");

extern const unsigned char webui_css_gz_start[]
    asm("_binary_generated_webui_assets_app_css_gz_start");
extern const unsigned char webui_css_gz_end[]
    asm("_binary_generated_webui_assets_app_css_gz_end");
#endif


static esp_err_t web_webui_flash_send(
    httpd_req_t *request,
    const unsigned char *start,
    const unsigned char *end,
    const char *content_type,
    bool gzip_encoded,
    bool text_file
)
{
    if (
        request == NULL ||
        start == NULL ||
        end == NULL ||
        end < start ||
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

    if (gzip_encoded) {
        httpd_resp_set_hdr(
            request,
            "Content-Encoding",
            "gzip"
        );
    }

    const size_t length =
        text_file
            ? strlen((const char *)start)
            : (size_t)(end - start);

    size_t offset =
        0;

    while (offset < length) {
        const size_t remaining =
            length - offset;

        const size_t chunk_length =
            remaining > WEB_WEBUI_FLASH_CHUNK_BYTES
                ? WEB_WEBUI_FLASH_CHUNK_BYTES
                : remaining;

        const esp_err_t err =
            httpd_resp_send_chunk(
                request,
                (const char *)start + offset,
                chunk_length
            );

        if (err != ESP_OK) {
            return err;
        }

        offset +=
            chunk_length;
    }

    return
        httpd_resp_send_chunk(
            request,
            NULL,
            0
        );
}


esp_err_t web_webui_flash_send_index(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(
        TAG,
        "React index served from flash"
    );

    return
        web_webui_flash_send(
            request,
            webui_index_start,
            webui_index_end,
            "text/html; charset=utf-8",
            false,
            true
        );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


static esp_err_t web_webui_flash_js_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(
        TAG,
        "React JS served from flash"
    );

    return
        web_webui_flash_send(
            request,
            webui_js_gz_start,
            webui_js_gz_end,
            "application/javascript; charset=utf-8",
            true,
            false
        );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


static esp_err_t web_webui_flash_css_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(
        TAG,
        "React CSS served from flash"
    );

    return
        web_webui_flash_send(
            request,
            webui_css_gz_start,
            webui_css_gz_end,
            "text/css; charset=utf-8",
            true,
            false
        );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


esp_err_t web_webui_flash_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    const httpd_uri_t routes[] = {
        {
            .uri = "/assets/app.js",
            .method = HTTP_GET,
            .handler = web_webui_flash_js_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/assets/app.css",
            .method = HTTP_GET,
            .handler = web_webui_flash_css_handler,
            .user_ctx = NULL,
        },
    };

    for (
        size_t index = 0;
        index < sizeof(routes) / sizeof(routes[0]);
        ++index
    ) {
        const esp_err_t err =
            httpd_register_uri_handler(
                server,
                &routes[index]
            );

        if (err != ESP_OK) {
            return err;
        }
    }
#endif

    return ESP_OK;
}
