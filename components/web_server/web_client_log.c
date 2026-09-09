#include "web_client_log.h"
#include "web_server_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define WEB_CLIENT_LOG_MAX_BODY_BYTES 4096U
#define WEB_CLIENT_LOG_MAX_LINES      32U

static const char *TAG = "WEB_CLIENT_LOG";

static void sanitize_line(char *line)
{
    if (line == NULL) {
        return;
    }

    for (char *cursor = line; *cursor != '\0'; ++cursor) {
        const unsigned char value = (unsigned char)*cursor;
        if (value < 0x20U || value == 0x7fU) {
            *cursor = ' ';
        }
    }
}

static esp_err_t web_client_log_handler(httpd_req_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t content_length = request->content_len;
    if (content_length == 0U || content_length > WEB_CLIENT_LOG_MAX_BODY_BYTES) {
        ESP_LOGW(TAG, "PHONE log batch rejected: bytes=%u", (unsigned)content_length);
        httpd_resp_set_status(request, "413 Payload Too Large");
        return httpd_resp_send(request, NULL, 0);
    }

    char *body = (char *)heap_caps_malloc(
        content_length + 1U,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (body == NULL) {
        ESP_LOGW(TAG, "PHONE log batch dropped: PSRAM allocation failed bytes=%u", (unsigned)content_length);
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_send(request, NULL, 0);
    }

    size_t offset = 0U;
    while (offset < content_length) {
        const int received = httpd_req_recv(
            request,
            body + offset,
            content_length - offset
        );
        if (received <= 0) {
            heap_caps_free(body);
            ESP_LOGW(TAG, "PHONE log receive failed after %u/%u bytes", (unsigned)offset, (unsigned)content_length);
            return received == HTTPD_SOCK_ERR_TIMEOUT
                ? ESP_ERR_TIMEOUT
                : ESP_FAIL;
        }
        offset += (size_t)received;
    }
    body[content_length] = '\0';

    unsigned line_count = 0U;
    char *save = NULL;
    for (
        char *line = strtok_r(body, "\n", &save);
        line != NULL && line_count < WEB_CLIENT_LOG_MAX_LINES;
        line = strtok_r(NULL, "\n", &save)
    ) {
        sanitize_line(line);
        if (line[0] != '\0') {
            ESP_LOGI(TAG, "PHONE %s", line);
            ++line_count;
        }
    }

    if (line_count >= WEB_CLIENT_LOG_MAX_LINES && save != NULL && *save != '\0') {
        ESP_LOGW(TAG, "PHONE log batch truncated at %u lines", line_count);
    }

    heap_caps_free(body);
    httpd_resp_set_status(request, "204 No Content");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

esp_err_t web_client_log_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t route = {
        .uri = "/api/client-log",
        .method = HTTP_POST,
        .handler = web_client_log_handler,
        .user_ctx = NULL,
    };

    const esp_err_t err = web_server_register_uri_handler(server, &route);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Registered POST /api/client-log (max %u bytes)", WEB_CLIENT_LOG_MAX_BODY_BYTES);
    }
    return err;
}
