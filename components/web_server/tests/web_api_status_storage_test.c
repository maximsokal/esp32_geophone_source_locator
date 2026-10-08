#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "object_store.h"
#include "storage_manager.h"
#include "web_server_internal.h"

static httpd_uri_func_t s_status_handler;
static unsigned s_build_calls;
static unsigned s_free_calls;
static bool s_last_include_objects;
static char s_last_session_object_id[OBJECT_STORE_ID_SIZE];
static esp_err_t s_build_result;
static char s_response_status[64];
static char s_response_type[96];
static char s_cache_control[32];
static char s_server_timing[96];
static char s_response_body[128];
static char s_json[] = "{\"system\":{\"state\":\"READY\"}}";
static int64_t s_now_us;

static void reset_fakes(void)
{
    s_status_handler = NULL;
    s_build_calls = 0U;
    s_free_calls = 0U;
    s_last_include_objects = false;
    s_last_session_object_id[0] = '\0';
    s_build_result = ESP_OK;
    s_response_status[0] = '\0';
    s_response_type[0] = '\0';
    s_cache_control[0] = '\0';
    s_server_timing[0] = '\0';
    s_response_body[0] = '\0';
    s_now_us = 1000;
    storage_manager_test_set_ready(true);
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    assert(server != NULL);
    assert(uri != NULL);
    assert(strcmp(uri->uri, "/api/status") == 0);
    assert(uri->method == HTTP_GET);
    s_status_handler = uri->handler;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    assert(uri != NULL);
    assert(strcmp(uri, "/api/status") == 0);
    assert(method == HTTP_GET);
    return ESP_OK;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    assert(request != NULL);
    return request->query == NULL ? 0U : strlen(request->query);
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t size)
{
    assert(request != NULL && buffer != NULL);
    if (request->query == NULL || strlen(request->query) + 1U > size) return ESP_FAIL;
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
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *equals = strchr(cursor, '=');
        if (equals == NULL) return ESP_FAIL;
        if ((size_t)(equals - cursor) == key_length &&
            strncmp(cursor, key, key_length) == 0) {
            const char *end = strchr(equals + 1, '&');
            const size_t length = end == NULL ? strlen(equals + 1) : (size_t)(end - equals - 1);
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

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    assert(request != NULL);
    if (strcmp(field, "Cache-Control") == 0) {
        snprintf(s_cache_control, sizeof(s_cache_control), "%s", value);
    } else if (strcmp(field, "Server-Timing") == 0) {
        snprintf(s_server_timing, sizeof(s_server_timing), "%s", value);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    assert(request != NULL);
    snprintf(s_response_status, sizeof(s_response_status), "%s", status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, ssize_t length)
{
    assert(request != NULL && body != NULL);
    assert(length == HTTPD_RESP_USE_STRLEN);
    snprintf(s_response_body, sizeof(s_response_body), "%s", body);
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)request;
    (void)status;
    (void)message;
    return ESP_FAIL;
}

int64_t esp_timer_get_time(void)
{
    return s_now_us++;
}

esp_err_t web_status_json_build_extended(
    char **json_out,
    bool include_objects,
    const char *sessions_object_id
)
{
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

static void register_status_handler(void)
{
    assert(web_api_status_register((httpd_handle_t)&s_status_handler) == ESP_OK);
    assert(s_status_handler != NULL);
}

static void assert_storage_unavailable_response(void)
{
    assert(strcmp(s_response_status, "503 Service Unavailable") == 0);
    assert(strcmp(s_response_type, "application/json; charset=utf-8") == 0);
    assert(strcmp(s_cache_control, "no-store") == 0);
    assert(strcmp(s_response_body, "{\"storage_unavailable\":true}") == 0);
    assert(strstr(s_server_timing, "storage=unavailable") != NULL);
}

static void test_objects_and_sessions_return_storage_unavailable(void)
{
    reset_fakes();
    register_status_handler();
    storage_manager_test_set_ready(false);
    httpd_req_t request = { .query = "objects=1" };
    assert(s_status_handler(&request) == ESP_OK);
    assert_storage_unavailable_response();
    assert(s_build_calls == 0U);

    reset_fakes();
    register_status_handler();
    storage_manager_test_set_ready(false);
    request.query = "sessions=OBJ_0001";
    assert(s_status_handler(&request) == ESP_OK);
    assert_storage_unavailable_response();
    assert(s_build_calls == 0U);
}

static void test_available_storage_cache_keeps_busy_contract(void)
{
    reset_fakes();
    register_status_handler();
    s_build_result = ESP_ERR_INVALID_STATE;
    httpd_req_t request = { .query = "objects=1" };
    assert(s_status_handler(&request) == ESP_OK);
    assert(strcmp(s_response_status, "503 Service Unavailable") == 0);
    assert(strcmp(s_response_body, "{\"storage_busy\":true}") == 0);
    assert(s_build_calls == 1U);
}

static void test_base_status_does_not_require_storage(void)
{
    reset_fakes();
    register_status_handler();
    storage_manager_test_set_ready(false);
    httpd_req_t request = { .query = NULL };
    assert(s_status_handler(&request) == ESP_OK);
    assert(s_build_calls == 1U);
    assert(s_free_calls == 1U);
    assert(strcmp(s_response_status, "") == 0);
    assert(strcmp(s_response_body, s_json) == 0);
}

int main(void)
{
    test_objects_and_sessions_return_storage_unavailable();
    test_available_storage_cache_keeps_busy_contract();
    test_base_status_does_not_require_storage();
    puts("web_api_status_storage_test: PASS");
    return 0;
}
