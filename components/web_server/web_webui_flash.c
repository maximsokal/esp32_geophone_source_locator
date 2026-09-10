#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#include "web_server_internal.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
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

#if defined(CONFIG_WEB_SERVER_WEBUI_PSRAM_CACHE) && \
    CONFIG_WEB_SERVER_WEBUI_PSRAM_CACHE
#define WEB_WEBUI_PSRAM_CACHE_ENABLED 1
#else
#define WEB_WEBUI_PSRAM_CACHE_ENABLED 0
#endif


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


#if WEB_SERVER_WEBUI_SEED_EMBEDDED && WEB_WEBUI_PSRAM_CACHE_ENABLED
typedef struct
{
    const unsigned char *flash_start;
    const unsigned char *flash_end;
    unsigned char *cached_start;
    size_t length;
} web_webui_psram_cache_entry_t;

static web_webui_psram_cache_entry_t s_psram_cache_entries[
    WEB_WEBUI_CHUNK_ASSET_COUNT + 2U
];
static size_t s_psram_cache_entry_count = 0U;
static unsigned char *s_psram_cache_blob = NULL;
static size_t s_psram_cache_bytes = 0U;
static bool s_psram_cache_ready = false;


static bool web_webui_psram_cache_add_flash_asset(
    const unsigned char *start,
    const unsigned char *end
)
{
    if (start == NULL || end == NULL || end < start) {
        return false;
    }

    for (size_t index = 0U; index < s_psram_cache_entry_count; ++index) {
        if (
            s_psram_cache_entries[index].flash_start == start &&
            s_psram_cache_entries[index].flash_end == end
        ) {
            return true;
        }
    }

    if (
        s_psram_cache_entry_count >=
        sizeof(s_psram_cache_entries) / sizeof(s_psram_cache_entries[0])
    ) {
        return false;
    }

    web_webui_psram_cache_entry_t *entry =
        &s_psram_cache_entries[s_psram_cache_entry_count++];
    entry->flash_start = start;
    entry->flash_end = end;
    entry->cached_start = NULL;
    entry->length = (size_t)(end - start);
    return true;
}


static esp_err_t web_webui_psram_cache_init(void)
{
    if (s_psram_cache_ready) {
        return ESP_OK;
    }

    s_psram_cache_entry_count = 0U;
    s_psram_cache_bytes = 0U;

    if (
        !web_webui_psram_cache_add_flash_asset(
            webui_js_gz_start,
            webui_js_gz_end
        ) ||
        !web_webui_psram_cache_add_flash_asset(
            webui_css_gz_start,
            webui_css_gz_end
        )
    ) {
        s_psram_cache_entry_count = 0U;
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t index = 0U; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        if (
            !web_webui_psram_cache_add_flash_asset(
                s_webui_chunk_assets[index].start,
                s_webui_chunk_assets[index].end
            )
        ) {
            s_psram_cache_entry_count = 0U;
            return ESP_ERR_INVALID_SIZE;
        }
    }

    for (size_t index = 0U; index < s_psram_cache_entry_count; ++index) {
        const size_t length = s_psram_cache_entries[index].length;
        if (length > SIZE_MAX - s_psram_cache_bytes) {
            s_psram_cache_entry_count = 0U;
            return ESP_ERR_INVALID_SIZE;
        }
        s_psram_cache_bytes += length;
    }

    if (s_psram_cache_bytes == 0U) {
        s_psram_cache_entry_count = 0U;
        return ESP_ERR_NOT_FOUND;
    }

    s_psram_cache_blob = heap_caps_malloc(
        s_psram_cache_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_psram_cache_blob == NULL) {
        ESP_LOGW(
            TAG,
            "WEBUI_PSRAM_CACHE allocation failed bytes=%u; using flash fallback",
            (unsigned)s_psram_cache_bytes
        );
        s_psram_cache_entry_count = 0U;
        s_psram_cache_bytes = 0U;
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0U;
    for (size_t index = 0U; index < s_psram_cache_entry_count; ++index) {
        web_webui_psram_cache_entry_t *entry =
            &s_psram_cache_entries[index];
        entry->cached_start = s_psram_cache_blob + offset;
        memcpy(entry->cached_start, entry->flash_start, entry->length);
        offset += entry->length;
    }

    s_psram_cache_ready = true;
    ESP_LOGI(
        TAG,
        "WEBUI_PSRAM_CACHE ready=1 assets=%u bytes=%u source=psram fallback=flash",
        (unsigned)s_psram_cache_entry_count,
        (unsigned)s_psram_cache_bytes
    );
    return ESP_OK;
}


static bool web_webui_psram_cache_lookup(
    const unsigned char *flash_start,
    const unsigned char *flash_end,
    const unsigned char **cached_start,
    const unsigned char **cached_end
)
{
    if (
        !s_psram_cache_ready ||
        flash_start == NULL ||
        flash_end == NULL ||
        cached_start == NULL ||
        cached_end == NULL
    ) {
        return false;
    }

    for (size_t index = 0U; index < s_psram_cache_entry_count; ++index) {
        const web_webui_psram_cache_entry_t *entry =
            &s_psram_cache_entries[index];
        if (
            entry->flash_start == flash_start &&
            entry->flash_end == flash_end
        ) {
            *cached_start = entry->cached_start;
            *cached_end = entry->cached_start + entry->length;
            return true;
        }
    }

    return false;
}
#endif


esp_err_t web_webui_flash_prepare_cache(void)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED && WEB_WEBUI_PSRAM_CACHE_ENABLED
    return web_webui_psram_cache_init();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


static esp_err_t web_webui_flash_send_with_source(
    httpd_req_t *request,
    const unsigned char *start,
    const unsigned char *end,
    const char *content_type,
    bool gzip_encoded,
    bool text_file,
    const char *cache_control,
    const char *source
)
{
    if (
        request == NULL ||
        start == NULL ||
        end == NULL ||
        end < start ||
        content_type == NULL ||
        cache_control == NULL ||
        source == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t length =
        text_file
            ? strlen((const char *)start)
            : (size_t)(end - start);

    const int64_t started_us =
        esp_timer_get_time();
    ESP_LOGI(
        TAG,
        "WEB_TIMING stage=asset_begin uri=%s source=%s bytes=%u ts_us=%lld gzip=%d",
        request->uri != NULL ? request->uri : "?",
        source,
        (unsigned)length,
        (long long)started_us,
        gzip_encoded ? 1 : 0
    );

    uint32_t elapsed_ms = 0U;
    esp_err_t err =
        httpd_resp_set_type(
            request,
            content_type
        );

    if (err != ESP_OK) {
        goto send_done;
    }

    err =
        httpd_resp_set_hdr(
            request,
            "Cache-Control",
            cache_control
        );

    if (err != ESP_OK) {
        goto send_done;
    }

    if (gzip_encoded) {
        err =
            httpd_resp_set_hdr(
                request,
                "Content-Encoding",
                "gzip"
            );

        if (err != ESP_OK) {
            goto send_done;
        }
    }

    ESP_LOGI(
        TAG,
        "FLASH_SEND_BEGIN uri=%s source=%s bytes=%u gzip=%d",
        request->uri != NULL ? request->uri : "?",
        source,
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

send_done:
    elapsed_ms =
        (uint32_t)(
            (esp_timer_get_time() - started_us) /
            1000
        );
    /* Keep the timing value observable even when host log stubs compile out. */
    (void)elapsed_ms;

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "FLASH_SEND_OK uri=%s source=%s bytes=%u elapsed=%u ms gzip=%d",
            request->uri != NULL ? request->uri : "?",
            source,
            (unsigned)length,
            (unsigned)elapsed_ms,
            gzip_encoded ? 1 : 0
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "FLASH_SEND_FAIL uri=%s source=%s bytes=%u elapsed=%u ms gzip=%d err=%s",
            request->uri != NULL ? request->uri : "?",
            source,
            (unsigned)length,
            (unsigned)elapsed_ms,
            gzip_encoded ? 1 : 0,
            esp_err_to_name(err)
        );
    }

    const int64_t ended_us = esp_timer_get_time();
    /* Keep the end timestamp observable even when host log stubs compile out. */
    (void)ended_us;
    ESP_LOGI(
        TAG,
        "WEB_TIMING stage=asset_end uri=%s source=%s bytes=%u ts_us=%lld elapsed_us=%lld result=%d",
        request->uri != NULL ? request->uri : "?",
        source,
        (unsigned)length,
        (long long)ended_us,
        (long long)(ended_us - started_us),
        (int)err
    );

    return err;
}


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
    return web_webui_flash_send_with_source(
        request,
        start,
        end,
        content_type,
        gzip_encoded,
        text_file,
        cache_control,
        "flash"
    );
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
        const unsigned char *start = webui_js_gz_start;
        const unsigned char *end = webui_js_gz_end;
        const char *source = "flash";
#if WEB_WEBUI_PSRAM_CACHE_ENABLED
        if (web_webui_psram_cache_lookup(
                webui_js_gz_start,
                webui_js_gz_end,
                &start,
                &end
            )) {
            source = "psram";
        }
#endif
        return
            web_webui_flash_send_with_source(
                request,
                start,
                end,
                "application/javascript; charset=utf-8",
                true,
                false,
                WEB_CACHE_NO_STORE,
                source
            );
    }

    if (web_webui_uri_equals(request->uri, "/assets/app.css")) {
        const unsigned char *start = webui_css_gz_start;
        const unsigned char *end = webui_css_gz_end;
        const char *source = "flash";
#if WEB_WEBUI_PSRAM_CACHE_ENABLED
        if (web_webui_psram_cache_lookup(
                webui_css_gz_start,
                webui_css_gz_end,
                &start,
                &end
            )) {
            source = "psram";
        }
#endif
        return
            web_webui_flash_send_with_source(
                request,
                start,
                end,
                "text/css; charset=utf-8",
                true,
                false,
                WEB_CACHE_NO_STORE,
                source
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

        const unsigned char *start = asset->start;
        const unsigned char *end = asset->end;
        const char *source = "flash";
#if WEB_WEBUI_PSRAM_CACHE_ENABLED
        if (web_webui_psram_cache_lookup(
                asset->start,
                asset->end,
                &start,
                &end
            )) {
            source = "psram";
        }
#endif

        ESP_LOGI(
            TAG,
            "React chunk served from %s: %s",
            source,
            asset->uri
        );

        return
            web_webui_flash_send_with_source(
                request,
                start,
                end,
                asset->content_type,
                true,
                false,
                WEB_CACHE_IMMUTABLE,
                source
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
        web_server_register_uri_handler(
            server,
            &asset_uri
        );
}
