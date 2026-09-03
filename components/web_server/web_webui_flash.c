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

#include "generated_webui_chunk_table.inc"
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

    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (gzip_encoded) {
        httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    }

    const size_t length =
        text_file
            ? strlen((const char *)start)
            : (size_t)(end - start);

    size_t offset = 0;
    while (offset < length) {
        const size_t remaining = length - offset;
        const size_t chunk_length =
            remaining > WEB_WEBUI_FLASH_CHUNK_BYTES
                ? WEB_WEBUI_FLASH_CHUNK_BYTES
                : remaining;
        const esp_err_t err = httpd_resp_send_chunk(
            request,
            (const char *)start + offset,
            chunk_length
        );
        if (err != ESP_OK) {
            return err;
        }
        offset += chunk_length;
    }

    return httpd_resp_send_chunk(request, NULL, 0);
}


static bool web_webui_uri_equals(
    const char *request_uri,
    const char *expected
)
{
    if (request_uri == NULL || expected == NULL) {
        return false;
    }
    const char *query = strchr(request_uri, '?');
    const size_t request_length =
        query == NULL
            ? strlen(request_uri)
            : (size_t)(query - request_uri);
    const size_t expected_length = strlen(expected);
    return
        request_length == expected_length &&
        strncmp(request_uri, expected, request_length) == 0;
}


esp_err_t web_webui_flash_send_index(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(TAG, "React index served from flash");
    return web_webui_flash_send(
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


static esp_err_t web_webui_flash_asset_handler(
    httpd_req_t *request
)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (web_webui_uri_equals(request->uri, "/assets/app.js")) {
        return web_webui_flash_send(
            request,
            webui_js_gz_start,
            webui_js_gz_end,
            "application/javascript; charset=utf-8",
            true,
            false
        );
    }
    if (web_webui_uri_equals(request->uri, "/assets/app.css")) {
        return web_webui_flash_send(
            request,
            webui_css_gz_start,
            webui_css_gz_end,
            "text/css; charset=utf-8",
            true,
            false
        );
    }

    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        const web_webui_chunk_asset_t *asset = &s_webui_chunk_assets[index];
        if (!web_webui_uri_equals(request->uri, asset->uri)) {
            continue;
        }
        ESP_LOGI(TAG, "React chunk served from flash: %s", asset->uri);
        return web_webui_flash_send(
            request,
            asset->start,
            asset->end,
            asset->content_type,
            true,
            false
        );
    }

    return httpd_resp_send_err(
        request,
        HTTPD_404_NOT_FOUND,
        "React Web UI asset unavailable"
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
    const httpd_uri_t asset_uri = {
        .uri = "/assets/*",
        .method = HTTP_GET,
        .handler = web_webui_flash_asset_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &asset_uri);
#else
    return ESP_OK;
#endif
}
