#include "web_server_internal.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEB_API_STATUS";

static bool web_api_status_include_objects(httpd_req_t *request)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= 64) return false;

    char query[64];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return false;

    char value[8];
    return httpd_query_key_value(query, "objects", value, sizeof(value)) == ESP_OK &&
        strcmp(value, "1") == 0;
}

static esp_err_t web_api_status_handler(httpd_req_t *request)
{
    char *json = NULL;
    esp_err_t err = web_status_json_build_extended(
        &json,
        web_api_status_include_objects(request)
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to build status JSON: %s", esp_err_to_name(err));
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to build device status"
        );
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
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

    esp_err_t err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /api/status: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Registered GET /api/status (?objects=1 for SD object read model)");
    return ESP_OK;
}
