#include "web_server_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "sd_card.h"


#define WEB_WEBUI_IO_CHUNK_BYTES  1024U
#define WEB_WEBUI_PATH_BYTES      192U


static const char *TAG =
    "WEB_WEBUI";

static const char *WEBUI_RELATIVE_ROOT =
    "/geolocator/web";


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


typedef enum
{
    WEB_WEBUI_ASSET_JS = 0,
    WEB_WEBUI_ASSET_CSS,

} web_webui_asset_t;


static bool web_webui_build_path(
    char *buffer,
    size_t buffer_size,
    const char *suffix
)
{
    if (
        buffer == NULL ||
        buffer_size == 0 ||
        suffix == NULL
    ) {
        return false;
    }

    const char *mount_point =
        sd_card_mount_point();

    if (mount_point == NULL) {
        return false;
    }

    const int written =
        snprintf(
            buffer,
            buffer_size,
            "%s%s%s",
            mount_point,
            WEBUI_RELATIVE_ROOT,
            suffix
        );

    return
        written > 0 &&
        (size_t)written < buffer_size;
}


static bool web_webui_file_exists(
    const char *path
)
{
    if (path == NULL) {
        return false;
    }

    struct stat info;

    return
        stat(
            path,
            &info
        ) == 0 &&
        S_ISREG(info.st_mode);
}


static esp_err_t web_webui_make_directory(
    const char *path
)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        mkdir(
            path,
            0775
        ) == 0 ||
        errno == EEXIST
    ) {
        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "mkdir failed for %s: errno=%d",
        path,
        errno
    );

    return ESP_FAIL;
}


static esp_err_t web_webui_prepare_directories(void)
{
    char path[
        WEB_WEBUI_PATH_BYTES
    ];

    const char *mount_point =
        sd_card_mount_point();

    if (mount_point == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err =
        web_webui_make_directory(
            mount_point
        );

    if (err != ESP_OK) {
        return err;
    }

    const int root_length =
        snprintf(
            path,
            sizeof(path),
            "%s/geolocator",
            mount_point
        );

    if (
        root_length <= 0 ||
        (size_t)root_length >= sizeof(path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        web_webui_make_directory(
            path
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            ""
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        web_webui_make_directory(
            path
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/assets"
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return
        web_webui_make_directory(
            path
        );
}


static esp_err_t web_webui_write_atomic(
    const char *path,
    const unsigned char *data,
    size_t length
)
{
    if (
        path == NULL ||
        data == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    char temporary_path[
        WEB_WEBUI_PATH_BYTES
    ];

    const int temporary_length =
        snprintf(
            temporary_path,
            sizeof(temporary_path),
            "%s.tmp",
            path
        );

    if (
        temporary_length <= 0 ||
        (size_t)temporary_length >= sizeof(temporary_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file =
        fopen(
            temporary_path,
            "wb"
        );

    if (file == NULL) {
        ESP_LOGE(
            TAG,
            "Could not open %s for write: errno=%d",
            temporary_path,
            errno
        );
        return ESP_FAIL;
    }

    const size_t written =
        fwrite(
            data,
            1,
            length,
            file
        );

    const int flush_result =
        fflush(
            file
        );

    const int close_result =
        fclose(
            file
        );

    if (
        written != length ||
        flush_result != 0 ||
        close_result != 0
    ) {
        ESP_LOGE(
            TAG,
            "Failed writing %s: expected=%u written=%u errno=%d",
            temporary_path,
            (unsigned)length,
            (unsigned)written,
            errno
        );

        (void)remove(
            temporary_path
        );

        return ESP_FAIL;
    }

    (void)remove(
        path
    );

    if (
        rename(
            temporary_path,
            path
        ) != 0
    ) {
        ESP_LOGE(
            TAG,
            "Could not replace %s: errno=%d",
            path,
            errno
        );

        (void)remove(
            temporary_path
        );

        return ESP_FAIL;
    }

    return ESP_OK;
}


static bool web_webui_sd_build_matches(void)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    char path[
        WEB_WEBUI_PATH_BYTES
    ];

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/build-id.txt"
        )
    ) {
        return false;
    }

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (file == NULL) {
        return false;
    }

    char current_build_id[64] =
        {0};

    const size_t read_length =
        fread(
            current_build_id,
            1,
            sizeof(current_build_id) - 1U,
            file
        );

    (void)fclose(
        file
    );

    current_build_id[
        read_length
    ] =
        '\0';

    current_build_id[
        strcspn(
            current_build_id,
            "\r\n"
        )
    ] =
        '\0';

    return
        strcmp(
            current_build_id,
            WEB_SERVER_WEBUI_BUILD_ID
        ) == 0;
#else
    return false;
#endif
}


static bool web_webui_sd_has_bundle(void)
{
    if (!sd_card_is_mounted()) {
        return false;
    }

    char path[
        WEB_WEBUI_PATH_BYTES
    ];

    static const char *required_suffixes[] = {
        "/index.html",
        "/assets/app.js.gz",
        "/assets/app.css.gz",
    };

    for (
        size_t index = 0;
        index < sizeof(required_suffixes) / sizeof(required_suffixes[0]);
        ++index
    ) {
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                required_suffixes[index]
            ) ||
            !web_webui_file_exists(
                path
            )
        ) {
            return false;
        }
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                s_webui_chunk_assets[index].sd_suffix
            ) ||
            !web_webui_file_exists(path)
        ) {
            return false;
        }
    }
#endif

    return true;
}


#if WEB_SERVER_WEBUI_SEED_EMBEDDED
static esp_err_t web_webui_seed_sd(void)
{
    if (!sd_card_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        web_webui_sd_build_matches() &&
        web_webui_sd_has_bundle()
    ) {
        ESP_LOGI(
            TAG,
            "SD Web UI already current: %s",
            WEB_SERVER_WEBUI_BUILD_ID
        );
        return ESP_OK;
    }

    esp_err_t err =
        web_webui_prepare_directories();

    if (err != ESP_OK) {
        return err;
    }

    char path[
        WEB_WEBUI_PATH_BYTES
    ];

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/index.html"
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        web_webui_write_atomic(
            path,
            webui_index_start,
            strlen(
                (const char *)webui_index_start
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/assets/app.js.gz"
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        web_webui_write_atomic(
            path,
            webui_js_gz_start,
            (size_t)(
                webui_js_gz_end -
                webui_js_gz_start
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/assets/app.css.gz"
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    err =
        web_webui_write_atomic(
            path,
            webui_css_gz_start,
            (size_t)(
                webui_css_gz_end -
                webui_css_gz_start
            )
        );

    if (err != ESP_OK) {
        return err;
    }

    for (size_t index = 0; index < WEB_WEBUI_CHUNK_ASSET_COUNT; ++index) {
        const web_webui_chunk_asset_t *asset = &s_webui_chunk_assets[index];
        if (
            !web_webui_build_path(
                path,
                sizeof(path),
                asset->sd_suffix
            )
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        err = web_webui_write_atomic(
            path,
            asset->start,
            (size_t)(asset->end - asset->start)
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    if (
        !web_webui_build_path(
            path,
            sizeof(path),
            "/build-id.txt"
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    static const char build_id[] =
        WEB_SERVER_WEBUI_BUILD_ID "\n";

    err =
        web_webui_write_atomic(
            path,
            (const unsigned char *)build_id,
            strlen(build_id)
        );

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(
        TAG,
        "Seeded React Web UI to SD: %s",
        WEB_SERVER_WEBUI_BUILD_ID
    );

    return ESP_OK;
}
#endif


static esp_err_t web_webui_stream_file(
    httpd_req_t *request,
    const char *path,
    const char *content_type,
    bool gzip_encoded
)
{
    if (
        request == NULL ||
        path == NULL ||
        content_type == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
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

    unsigned char buffer[
        WEB_WEBUI_IO_CHUNK_BYTES
    ];

    esp_err_t result =
        ESP_OK;

    for (;;) {
        const size_t read_length =
            fread(
                buffer,
                1,
                sizeof(buffer),
                file
            );

        if (read_length > 0) {
            result =
                httpd_resp_send_chunk(
                    request,
                    (const char *)buffer,
                    read_length
                );

            if (result != ESP_OK) {
                break;
            }
        }

        if (read_length < sizeof(buffer)) {
            if (ferror(file)) {
                result =
                    ESP_FAIL;
            }
            break;
        }
    }

    (void)fclose(
        file
    );

    if (result != ESP_OK) {
        return result;
    }

    return
        httpd_resp_send_chunk(
            request,
            NULL,
            0
        );
}


static esp_err_t web_webui_send_embedded(
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
        content_type == NULL ||
        end < start
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
            remaining > WEB_WEBUI_IO_CHUNK_BYTES
                ? WEB_WEBUI_IO_CHUNK_BYTES
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


esp_err_t web_webui_prepare(void)
{
#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    if (sd_card_is_mounted()) {
        const esp_err_t err =
            web_webui_seed_sd();

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "SD Web UI seed failed, flash fallback remains available: %s",
                esp_err_to_name(err)
            );
            return ESP_OK;
        }
    } else {
        ESP_LOGW(
            TAG,
            "SD not mounted; React Web UI will be served from flash"
        );
    }

    return ESP_OK;
#else
    if (web_webui_sd_has_bundle()) {
        ESP_LOGI(
            TAG,
            "Using pre-existing React Web UI bundle from SD"
        );
    } else {
        ESP_LOGW(
            TAG,
            "React Web UI unavailable; legacy embedded dashboard will remain root"
        );
    }

    return ESP_OK;
#endif
}


esp_err_t web_webui_send_index(
    httpd_req_t *request
)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sd_card_is_mounted()) {
        char path[
            WEB_WEBUI_PATH_BYTES
        ];

        if (
            web_webui_build_path(
                path,
                sizeof(path),
                "/index.html"
            ) &&
            web_webui_file_exists(
                path
            )
        ) {
            const esp_err_t err =
                web_webui_stream_file(
                    request,
                    path,
                    "text/html; charset=utf-8",
                    false
                );

            if (err == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "React index served from SD"
                );
                return ESP_OK;
            }

            ESP_LOGW(
                TAG,
                "Could not serve React index from SD: %s",
                esp_err_to_name(err)
            );
        }
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    ESP_LOGI(
        TAG,
        "React index served from flash"
    );

    return
        web_webui_send_embedded(
            request,
            webui_index_start,
            webui_index_end,
            "text/html; charset=utf-8",
            false,
            true
        );
#else
    return ESP_ERR_NOT_FOUND;
#endif
}


static esp_err_t web_webui_send_asset(
    httpd_req_t *request,
    web_webui_asset_t asset
)
{
    const char *suffix =
        asset == WEB_WEBUI_ASSET_JS
            ? "/assets/app.js.gz"
            : "/assets/app.css.gz";

    const char *content_type =
        asset == WEB_WEBUI_ASSET_JS
            ? "application/javascript; charset=utf-8"
            : "text/css; charset=utf-8";

    if (sd_card_is_mounted()) {
        char path[
            WEB_WEBUI_PATH_BYTES
        ];

        if (
            web_webui_build_path(
                path,
                sizeof(path),
                suffix
            ) &&
            web_webui_file_exists(
                path
            )
        ) {
            const esp_err_t err =
                web_webui_stream_file(
                    request,
                    path,
                    content_type,
                    true
                );

            if (err == ESP_OK) {
                return ESP_OK;
            }

            ESP_LOGW(
                TAG,
                "Could not serve %s from SD: %s",
                suffix,
                esp_err_to_name(err)
            );
        }
    }

#if WEB_SERVER_WEBUI_SEED_EMBEDDED
    if (asset == WEB_WEBUI_ASSET_JS) {
        return
            web_webui_send_embedded(
                request,
                webui_js_gz_start,
                webui_js_gz_end,
                content_type,
                true,
                false
            );
    }

    return
        web_webui_send_embedded(
            request,
            webui_css_gz_start,
            webui_css_gz_end,
            content_type,
            true,
            false
        );
#else
    return
        httpd_resp_send_err(
            request,
            HTTPD_404_NOT_FOUND,
            "React Web UI asset unavailable"
        );
#endif
}


static esp_err_t web_webui_js_handler(
    httpd_req_t *request
)
{
    return
        web_webui_send_asset(
            request,
            WEB_WEBUI_ASSET_JS
        );
}


static esp_err_t web_webui_css_handler(
    httpd_req_t *request
)
{
    return
        web_webui_send_asset(
            request,
            WEB_WEBUI_ASSET_CSS
        );
}


esp_err_t web_webui_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t routes[] = {
        {
            .uri = "/assets/app.js",
            .method = HTTP_GET,
            .handler = web_webui_js_handler,
            .user_ctx = NULL,
        },
        {
            .uri = "/assets/app.css",
            .method = HTTP_GET,
            .handler = web_webui_css_handler,
            .user_ctx = NULL,
        },
    };

    for (
        size_t index = 0;
        index < sizeof(routes) / sizeof(routes[0]);
        ++index
    ) {
        const esp_err_t err =
            web_server_register_uri_handler(
                server,
                &routes[index]
            );

        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}
