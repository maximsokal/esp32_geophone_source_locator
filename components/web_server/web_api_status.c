#include "web_server_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "object_store.h"
#if defined(ESP_PLATFORM)
#include "storage_manager.h"
#else
#include <stdbool.h>
bool storage_manager_is_ready(void);
#endif

#define WEB_STATUS_SLOW_BUILD_US 100000LL

static const char *TAG = "WEB_API_STATUS";

typedef struct
{
    bool include_objects;
    char sessions_object_id[OBJECT_STORE_ID_SIZE];
} web_status_query_t;


static esp_err_t web_api_status_send_storage_busy(
    httpd_req_t *request,
    int64_t build_us
)
{
    char timing[64];
    (void)snprintf(
        timing,
        sizeof(timing),
        "status-build;dur=%.1f;storage=busy",
        (double)build_us / 1000.0
    );
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Server-Timing", timing);
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(
        request,
        "{\"storage_busy\":true}",
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t web_api_status_send_storage_unavailable(
    httpd_req_t *request,
    int64_t build_us
)
{
    char timing[64];
    (void)snprintf(
        timing,
        sizeof(timing),
        "status-build;dur=%.1f;storage=unavailable",
        (double)build_us / 1000.0
    );
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Server-Timing", timing);
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(
        request,
        "{\"storage_unavailable\":true}",
        HTTPD_RESP_USE_STRLEN
    );
}

static void web_api_status_parse_query(
    httpd_req_t *request,
    web_status_query_t *result
)
{
    memset(result, 0, sizeof(*result));

    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= 128) return;

    char query[128];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return;

    char objects[8];
    result->include_objects =
        httpd_query_key_value(query, "objects", objects, sizeof(objects)) == ESP_OK &&
        strcmp(objects, "1") == 0;

    (void)httpd_query_key_value(
        query,
        "sessions",
        result->sessions_object_id,
        sizeof(result->sessions_object_id)
    );
}

static esp_err_t web_api_status_handler(httpd_req_t *request)
{
    web_status_query_t query;
    web_api_status_parse_query(request, &query);

    const int64_t started_us = esp_timer_get_time();
    const bool storage_query =
        query.include_objects || query.sessions_object_id[0] != '\0';
    if (storage_query && !storage_manager_is_ready()) {
        const int64_t build_us = esp_timer_get_time() - started_us;
        return web_api_status_send_storage_unavailable(request, build_us);
    }

    char *json = NULL;
    esp_err_t err = web_status_json_build_extended(
        &json,
        query.include_objects,
        query.sessions_object_id[0] == '\0'
            ? NULL
            : query.sessions_object_id
    );
    const int64_t build_us = esp_timer_get_time() - started_us;

    if (err != ESP_OK) {
        if (
            err == ESP_ERR_INVALID_STATE &&
            (query.include_objects || query.sessions_object_id[0] != '\0')
        ) {
            ESP_LOGW(
                TAG,
                "Status storage cache unavailable after %lld ms; returning 503",
                build_us / 1000LL
            );
            return web_api_status_send_storage_busy(request, build_us);
        }

        ESP_LOGE(
            TAG,
            "Failed to build status JSON after %lld ms: %s",
            build_us / 1000LL,
            esp_err_to_name(err)
        );
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to build device status"
        );
    }

    if (build_us >= WEB_STATUS_SLOW_BUILD_US) {
        ESP_LOGW(
            TAG,
            "Slow status build: %lld ms objects=%d sessions=%s",
            build_us / 1000LL,
            query.include_objects ? 1 : 0,
            query.sessions_object_id[0] == '\0' ? "none" : query.sessions_object_id
        );
    }

    char timing[48];
    (void)snprintf(
        timing,
        sizeof(timing),
        "status-build;dur=%.1f",
        (double)build_us / 1000.0
    );

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Server-Timing", timing);
    err = httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
    web_status_json_free(json);
    return err;
}

esp_err_t web_api_status_register(httpd_handle_t server)
{
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = web_api_status_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = web_server_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/status: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(
        TAG,
        "Registered GET /api/status (?objects=1, ?sessions=OBJECT_ID from RAM/PSRAM cache; 503 while refreshing)"
    );
    return ESP_OK;
}
