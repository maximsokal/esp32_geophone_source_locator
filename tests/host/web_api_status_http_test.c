#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "object_store.h"
#include "web_server_internal.h"

static httpd_uri_func_t s_status_handler;
static esp_err_t s_register_result;
static unsigned s_register_calls;
static esp_err_t s_tracking_result;
static esp_err_t s_build_result;
static bool s_last_include_objects;
static char s_last_session_object_id[OBJECT_STORE_ID_SIZE];
static unsigned s_build_calls;
static unsigned s_free_calls;
static char s_json[] = "{\"system\":{\"state\":\"READY\"}}";
static unsigned s_timer_calls;
static int64_t s_first_time_us;
static int64_t s_second_time_us;
static char s_response_type[96];
static char s_response_status[64];
static char s_header_cache_control[64];
static char s_header_server_timing[96];
static char s_response_body[256];
static int s_error_status;
static char s_error_message[96];
static esp_err_t s_send_result;

static void reset_fake(void)
{
    s_register_result = ESP_OK;
    s_register_calls = 0U;
    s_tracking_result = ESP_OK;
    s_build_result = ESP_OK;
    s_last_include_objects = false;
    s_last_session_object_id[0] = '\0';
    s_build_calls = 0U;
    s_free_calls = 0U;
    s_timer_calls = 0U;
    s_first_time_us = 1000;
    s_second_time_us = 2000;
    s_response_type[0] = '\0';
    s_response_status[0] = '\0';
    s_header_cache_control[0] = '\0';
    s_header_server_timing[0] = '\0';
    s_response_body[0] = '\0';
    s_error_status = 0;
    s_error_message[0] = '\0';
    s_send_result = ESP_OK;
}

esp_err_t httpd_register_uri_handler(
    httpd_handle_t server,
    const httpd_uri_t *uri
)
{
    assert(server != NULL);
    assert(uri != NULL);
    ++s_register_calls;
    if (s_register_result == ESP_OK) {
        assert(strcmp(uri->uri, "/api/status") == 0);
        assert(uri->method == HTTP_GET);
        s_status_handler = uri->handler;
    }
    return s_register_result;
}

esp_err_t web_server_route_tracking_register(
    const char *uri,
    int method
)
{
    assert(uri != NULL);
    assert(strcmp(uri, "/api/status") == 0);
    assert(method == HTTP_GET);
    return s_tracking_result;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    assert(request != NULL);
    return request->query == NULL ? 0U : strlen(request->query);
}

esp_err_t httpd_req_get_url_query_str(
    httpd_req_t *request,
    char *buffer,
    size_t buffer_size
)
{
    assert(request != NULL);
    assert(buffer != NULL);
    if (request->query == NULL || strlen(request->query) + 1U > buffer_size) {
        return ESP_FAIL;
    }
    memcpy(buffer, request->query, strlen(request->query) + 1U);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
)
{
    assert(query != NULL);
    assert(key != NULL);
    assert(value != NULL);
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *equals = strchr(cursor, '=');
        if (equals == NULL) return ESP_FAIL;
        if ((size_t)(equals - cursor) == key_length &&
            strncmp(cursor, key, key_length) == 0) {
            const char *end = strchr(equals + 1, '&');
            const size_t length = end == NULL
                ? strlen(equals + 1)
                : (size_t)(end - (equals + 1));
            if (length + 1U > value_size) return ESP_FAIL;
            memcpy(value, equals + 1, length);
            value[length] = '\0';
            return ESP_OK;
        }
        cursor = strchr(equals + 1, '&');
        if (cursor == NULL) return ESP_FAIL;
        ++cursor;
    }
    return ESP_FAIL;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    assert(request != NULL);
    snprintf(s_response_type, sizeof(s_response_type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(
    httpd_req_t *request,
    const char *field,
    const char *value
)
{
    assert(request != NULL);
    if (strcmp(field, "Cache-Control") == 0) {
        snprintf(s_header_cache_control, sizeof(s_header_cache_control), "%s", value);
    }
    if (strcmp(field, "Server-Timing") == 0) {
        snprintf(s_header_server_timing, sizeof(s_header_server_timing), "%s", value);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    assert(request != NULL);
    snprintf(s_response_status, sizeof(s_response_status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(
    httpd_req_t *request,
    const char *buffer,
    ssize_t length
)
{
    assert(request != NULL);
    assert(length == HTTPD_RESP_USE_STRLEN);
    assert(buffer != NULL);
    snprintf(s_response_body, sizeof(s_response_body), "%s", buffer);
    return s_send_result;
}

esp_err_t httpd_resp_send_err(
    httpd_req_t *request,
    int status,
    const char *message
)
{
    assert(request != NULL);
    s_error_status = status;
    snprintf(s_error_message, sizeof(s_error_message), "%s", message);
    return ESP_FAIL;
}

int64_t esp_timer_get_time(void)
{
    return s_timer_calls++ == 0U ? s_first_time_us : s_second_time_us;
}

esp_err_t web_status_json_build_extended(
    char **json_out,
    bool include_objects,
    const char *sessions_object_id
)
{
    assert(json_out != NULL);
    ++s_build_calls;
    s_last_include_objects = include_objects;
    snprintf(
        s_last_session_object_id,
        sizeof(s_last_session_object_id),
        "%s",
        sessions_object_id == NULL ? "" : sessions_object_id
    );
    *json_out = s_build_result == ESP_OK ? s_json : NULL;
    return s_build_result;
}

void web_status_json_free(char *json)
{
    assert(json == s_json);
    ++s_free_calls;
}

static void test_registration_and_normal_status(void)
{
    reset_fake();
    assert(web_api_status_register((httpd_handle_t)&s_status_handler) == ESP_OK);
    assert(s_register_calls == 1U);
    assert(s_status_handler != NULL);

    httpd_req_t request = { .query = NULL };
    assert(s_status_handler(&request) == ESP_OK);
    assert(s_build_calls == 1U);
    assert(!s_last_include_objects);
    assert(s_last_session_object_id[0] == '\0');
    assert(strcmp(s_response_type, "application/json; charset=utf-8") == 0);
    assert(strcmp(s_header_cache_control, "no-store") == 0);
    assert(strcmp(s_header_server_timing, "status-build;dur=1.0") == 0);
    assert(strcmp(s_response_body, s_json) == 0);
    assert(s_free_calls == 1U);
    assert(s_error_status == 0);
}

static void test_extended_query_is_forwarded(void)
{
    reset_fake();
    httpd_req_t request = { .query = "objects=1&sessions=OBJ_0007" };
    assert(s_status_handler(&request) == ESP_OK);
    assert(s_last_include_objects);
    assert(strcmp(s_last_session_object_id, "OBJ_0007") == 0);

    char long_query[129];
    memset(long_query, 'x', sizeof(long_query) - 1U);
    long_query[sizeof(long_query) - 1U] = '\0';
    reset_fake();
    request.query = long_query;
    assert(s_status_handler(&request) == ESP_OK);
    assert(!s_last_include_objects);
    assert(s_last_session_object_id[0] == '\0');
}

static void test_storage_busy_and_build_errors_are_distinct(void)
{
    httpd_req_t request = { .query = "objects=1" };

    reset_fake();
    s_build_result = ESP_ERR_INVALID_STATE;
    assert(s_status_handler(&request) == ESP_OK);
    assert(strcmp(s_response_status, "503 Service Unavailable") == 0);
    assert(strcmp(s_response_body, "{\"storage_busy\":true}") == 0);
    assert(s_error_status == 0);

    reset_fake();
    request.query = NULL;
    s_build_result = ESP_ERR_INVALID_STATE;
    assert(s_status_handler(&request) == ESP_FAIL);
    assert(s_error_status == HTTPD_500_INTERNAL_SERVER_ERROR);
    assert(strcmp(s_error_message, "Failed to build device status") == 0);

    reset_fake();
    s_build_result = ESP_FAIL;
    assert(s_status_handler(&request) == ESP_FAIL);
    assert(s_error_status == HTTPD_500_INTERNAL_SERVER_ERROR);
}

static void test_response_error_still_frees_json(void)
{
    reset_fake();
    s_send_result = ESP_FAIL;
    httpd_req_t request = { .query = NULL };
    assert(s_status_handler(&request) == ESP_FAIL);
    assert(s_free_calls == 1U);
}

static void test_registration_errors(void)
{
    reset_fake();
    assert(web_api_status_register(NULL) == ESP_ERR_INVALID_ARG);

    s_register_result = ESP_FAIL;
    assert(web_api_status_register((httpd_handle_t)&s_status_handler) == ESP_FAIL);

    reset_fake();
    s_tracking_result = ESP_FAIL;
    assert(web_api_status_register((httpd_handle_t)&s_status_handler) == ESP_FAIL);
}

int main(void)
{
    test_registration_and_normal_status();
    test_extended_query_is_forwarded();
    test_storage_busy_and_build_errors_are_distinct();
    test_response_error_still_frees_json();
    test_registration_errors();
    puts("web API status HTTP tests passed");
    return 0;
}
