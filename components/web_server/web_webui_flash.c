#include "web_server_internal.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "WEB_WEBUI_FLASH";

static const char *WEB_CACHE_NO_STORE =
    "no-store";

static const char *WEB_CACHE_IMMUTABLE =
    "public, max-age=31536000, immutable";

/*
 * Keep individual TLS writes small. The ESP32-S3 has PSRAM, but the Wi-Fi,
 * HTTPD socket and SDSPI paths still compete for scarce internal RAM. A 2 KiB
 * payload avoids one long blocking 10+ KiB mbedTLS write while requiring only
 * a handful of records for the small dashboard entry bundle.
 */
#define WEB_FLASH_SEND_CHUNK_BYTES  2048U


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
    bool text_file,
    const char *cache_control
)
{
    if (
        request == NULL ||
        start == NULL ||
        end == NULL ||
        end < start ||
        content_type == NULL ||
        cache_control == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length =
        text_file
            ? strlen((const char *)start)
            : (size_t)(end - start);

    esp_err_t err =
        httpd_resp_set_type(
            request,
            content_type
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        httpd_resp_set_hdr(
            request,
            "Cache-Control",
            cache_control
        );

    if (err != ESP_OK) {
        return err;
    }

    /*
     * Do not leave a TLS client parked in HTTP keep-alive after an asset has
     * been delivered. A persistent browser session consumes enough internal
     * RAM to starve the SDSPI 512-byte DMA bounce allocation. Session tickets
     * make the next HTTPS connection cheap while Connection: close returns the
     * TLS client memory to the device between requests.
     */
    err =
        httpd_resp_set_hdr(
            request,
            "Connection",
            "close"
        );

    if (err != ESP_OK) {
        return err;
    }

    if (gzip_encoded) {
        err =
            httpd_resp_set_hdr(
                request,
                "Content-Encoding",
                "gzip"
            );

        if (err != ESP_OK) {
            return err;
        }
    }

    const int64_t started_us =
        esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "FLASH_SEND_BEGIN uri=%s bytes=%u gzip=%d",
        request->uri != NULL ? request->uri : "?",
        (unsigned)length,
        gzip_encoded ? 1 : 0
    );

    if (length <= WEB_FLASH_SEND_CHUNK_BYTES) {
        err =
            httpd_resp_send(
                request,
                (const char *)start,
                (ssize_t)length
            );
    }
    else {
        size_t offset = 0U;

        while (
            err == ESP_OK &&
            offset < length
        ) {
            size_t chunk_length =
                length - offset;

            if (chunk_length > WEB_FLASH_SEND_CHUNK_BYTES) {
                chunk_length = WEB_FLASH_SEND_CHUNK_BYTES;
            }

            err =
                httpd_resp_send_chunk(
                    request,
                    (const char *)(start + offset),
                    (ssize_t)chunk_length
                );

            if (err == ESP_OK) {
                offset += chunk_length;
                taskYIELD();
            }
        }

        if (err == ESP_OK) {
            err =
                httpd_resp_send_chunk(
                    request,
                    NULL,
                    0
                );
        }
    }

    const uint32_t elapsed_ms =
        (uint32_t)(
            (esp_timer_get_time() - started_us) /
            1000
        );
    /* Keep the timing value observable even when host log stubs compile out. */
    (void)elapsed_ms;

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "FLASH_SEND_OK uri=%s bytes=%u elapsed=%u ms gzip=%d",
            request->uri != NULL ? request->uri : "?",
            (unsigned)length,
            (unsigned)elapsed_ms,
            gzip_encoded ? 1 : 0
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "FLASH_SEND_FAIL uri=%s bytes=%u elapsed=%u ms gzip=%d err=%s",
            request->uri != NULL ? request->uri : "?",
            (unsigned)length,
            (unsigned)elapsed_ms,
            gzip_encoded ? 1 : 0,
            esp_err_to_name(err)
        );
    }

    return err;
}


static bool web_webui_uri_equals(
    const char *request_uri,
    const char *expected
)
{
    if (
        request_uri == NULL ||
        expected == NULL
    ) {
        return false;
    }

    const char *query =
        strchr(
            request_uri,
            '?'
        );

    const size_t request_length =
        query == NULL
            ? strlen(request_uri)
            : (size_t)(query - request_uri);

    const size_t expected_length =
        strlen(expected);

    return
        request_length == expected_length &&
        strncmp(
            request_uri,
            expected,
            request_length
        ) == 0;
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
            true,
            WEB_CACHE_NO_STORE
        );
#else
    (void)request;
    return ESP_ERR_NOT_FOUND;
#endif
}


typedef struct
{
    const char *uri;
    web_static_asset_id_t id;
    const char *content_type;
} web_legacy_asset_route_t;


static esp_err_t web_webui_flash_send_legacy_asset(
    httpd_req_t *request
)
{
    static const web_legacy_asset_route_t routes[] = {
        {
            "/assets/dashboard.css",
            WEB_STATIC_ASSET_DASHBOARD_CSS,
            "text/css; charset=utf-8"
        },
        {
            "/assets/dashboard.js",
            WEB_STATIC_ASSET_DASHBOARD_JS,
            "application/javascript; charset=utf-8"
        },
        {
            "/assets/seismic.css",
            WEB_STATIC_ASSET_SEISMIC_CSS,
            "text/css; charset=utf-8"
        },
        {
            "/assets/seismic.js",
            WEB_STATIC_ASSET_SEISMIC_JS,
            "application/javascript; charset=utf-8"
        },
    };

    for (
        size_t i = 0;
        i < sizeof(routes) / sizeof(routes[0]);
        ++i
    ) {
        if (!web_webui_uri_equals(request->uri, routes[i].uri)) {
            continue;
        }

        const char *data =
            web_static_asset_data(
                routes[i].id
            );

        if (data == NULL) {
            return ESP_ERR_NOT_FOUND;
        }

        return
            web_webui_flash_send(
                request,
                (const unsigned char *)data,
                (const unsigned char *)data + strlen(data),
                routes[i].content_type,
                false,
                true,
                WEB_CACHE_NO_STORE
            );
    }

    return ESP_ERR_NOT_FOUND;
}


static esp_err_t web_webui_flash_asset_handler(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    /*
     * app.js/app.css keep no-store because their stable path is versioned by
     * the index query string. Hashed secondary chunks are content-addressed
     * and can safely stay in the browser cache across reloads.
     */
    if (web_webui_uri_equals(request->uri, "/assets/app.js")) {
        return
            web_webui_flash_send(
                request,
                webui_js_gz_start,
                webui_js_gz_end,
                "application/javascript; charset=utf-8",
                true,
                false,
                WEB_CACHE_NO_STORE
            );
    }

    if (web_webui_uri_equals(request->uri, "/assets/app.css")) {
        return
            web_webui_flash_send(
                request,
                webui_css_gz_start,
                webui_css_gz_end,
                "text/css; charset=utf-8",
                true,
                false,
                WEB_CACHE_NO_STORE
            );
    }

    for (
        size_t index = 0;
        index < WEB_WEBUI_CHUNK_ASSET_COUNT;
        ++index
    ) {
        const web_webui_chunk_asset_t *asset =
            &s_webui_chunk_assets[index];

        if (!web_webui_uri_equals(request->uri, asset->uri)) {
            continue;
        }

        ESP_LOGI(
            TAG,
            "React chunk served from flash: %s",
            asset->uri
        );

        return
            web_webui_flash_send(
                request,
                asset->start,
                asset->end,
                asset->content_type,
                true,
                false,
                WEB_CACHE_IMMUTABLE
            );
    }
#endif

    const esp_err_t legacy =
        web_webui_flash_send_legacy_asset(
            request
        );

    if (legacy != ESP_ERR_NOT_FOUND) {
        return legacy;
    }

    return
        httpd_resp_send_err(
            request,
            HTTPD_404_NOT_FOUND,
            "Web UI asset unavailable"
        );
}


esp_err_t web_webui_flash_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t asset_uri = {
        .uri =
            "/assets/*",

        .method =
            HTTP_GET,

        .handler =
            web_webui_flash_asset_handler,

        .user_ctx =
            NULL,
    };

    return
        httpd_register_uri_handler(
            server,
            &asset_uri
        );
}
