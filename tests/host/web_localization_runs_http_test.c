#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "localization_run_record.h"
#include "localization_run_store.h"
#include "object_store.h"
#include "web_server_internal.h"

static unsigned s_list_calls;
static esp_err_t s_list_result;
static char s_list_object_id[OBJECT_STORE_ID_SIZE];
static char s_list_session_id[LOCALIZATION_RUN_SESSION_ID_SIZE];

static void reset_fakes(void)
{
    s_list_calls = 0U;
    s_list_result = ESP_OK;
    memset(s_list_object_id, 0, sizeof(s_list_object_id));
    memset(s_list_session_id, 0, sizeof(s_list_session_id));
}

bool power_control_is_shutdown_requested(void)
{
    return false;
}

esp_err_t localization_run_store_list(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out
)
{
    assert(records != NULL && record_count_out != NULL && max_records >= 2U);
    s_list_calls++;
    snprintf(s_list_object_id, sizeof(s_list_object_id), "%s", object_id);
    snprintf(s_list_session_id, sizeof(s_list_session_id), "%s", session_id);

    *record_count_out = 0U;
    if (s_list_result != ESP_OK) {
        return s_list_result;
    }

    memset(&records[0], 0, sizeof(records[0]));
    records[0].schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    snprintf(records[0].object_id, sizeof(records[0].object_id), "OBJ_0001");
    snprintf(records[0].session_id, sizeof(records[0].session_id), "S000001");
    records[0].computed_at_ms = 123456ULL;
    records[0].kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(records[0].method_name, sizeof(records[0].method_name), "tdoa_gauss_newton");
    records[0].dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    records[0].solved = true;
    records[0].source_position = (localization_point3d_t) { 1.0, 2.0, 3.0 };
    records[0].confidence_score = 0.8;

    memset(&records[1], 0, sizeof(records[1]));
    records[1].schema_version = LOCALIZATION_RUN_SCHEMA_VERSION;
    snprintf(records[1].object_id, sizeof(records[1].object_id), "OBJ_0001");
    snprintf(records[1].session_id, sizeof(records[1].session_id), "S000001");
    records[1].computed_at_ms = 123456ULL;
    records[1].kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(records[1].method_name, sizeof(records[1].method_name), "semblance");
    records[1].dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    records[1].solved = false;
    snprintf(
        records[1].rejection_reason, sizeof(records[1].rejection_reason),
        "semblance search found no coherent candidate"
    );

    *record_count_out = 2U;
    return ESP_OK;
}

static esp_err_t query_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
)
{
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, '&');
        const char *equals = strchr(cursor, '=');
        if (equals != NULL && (end == NULL || equals < end)) {
            const size_t candidate_length = (size_t)(equals - cursor);
            if (candidate_length == key_length && strncmp(cursor, key, key_length) == 0) {
                const char *start = equals + 1;
                const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
                if (length == 0U || length >= value_size) return ESP_ERR_INVALID_SIZE;
                memcpy(value, start, length);
                value[length] = '\0';
                return ESP_OK;
            }
        }
        if (end == NULL) break;
        cursor = end + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t httpd_req_get_url_query_len(httpd_req_t *request)
{
    return request != NULL && request->query != NULL ? strlen(request->query) : 0U;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *request, char *buffer, size_t buffer_size)
{
    if (request == NULL || request->query == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(request->query);
    if (length >= buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, request->query, length + 1U);
    return ESP_OK;
}

esp_err_t httpd_query_key_value(const char *query, const char *key, char *value, size_t value_size)
{
    return query_value(query, key, value, value_size);
}

int httpd_req_recv(httpd_req_t *request, void *buffer, size_t length)
{
    (void)request;
    (void)buffer;
    (void)length;
    return ESP_FAIL;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    request->content_type = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *field, const char *value)
{
    if (strcmp(field, "Cache-Control") == 0) request->cache_control = value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request->response_status = status;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)message;
    if (status == 400) request->response_status = "400 Bad Request";
    else if (status == 404) request->response_status = "404 Not Found";
    else if (status == 422) request->response_status = "422 Unprocessable Entity";
    else request->response_status = "500 Internal Server Error";
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t length)
{
    const size_t bytes = length == HTTPD_RESP_USE_STRLEN ? strlen(buffer) : (size_t)length;
    assert(request != NULL && request->response_body != NULL);
    assert(bytes <= request->response_capacity);
    memcpy(request->response_body, buffer, bytes);
    request->response_length = bytes;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    if (buffer == NULL && length == 0) return ESP_OK;
    assert(request != NULL && buffer != NULL && length > 0);
    assert(request->response_length + (size_t)length <= request->response_capacity);
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    (void)uri;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)uri;
    (void)method;
    return ESP_OK;
}

#define static
#include "../../components/web_server/web_localization_api.c"
#undef static

static httpd_req_t make_request(const char *query, char *response, size_t response_capacity)
{
    return (httpd_req_t) {
        .uri = "/api/localization/runs",
        .query = query,
        .response_body = response,
        .response_capacity = response_capacity,
    };
}

static void test_runs_listing_round_trips_persisted_records(void)
{
    reset_fakes();
    char response[2048] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(s_list_calls == 1U);
    assert(strcmp(s_list_object_id, "OBJ_0001") == 0);
    assert(strcmp(s_list_session_id, "S000001") == 0);
    assert(strstr(response, "\"object_id\":\"OBJ_0001\"") != NULL);
    assert(strstr(response, "\"session_id\":\"S000001\"") != NULL);
    assert(strstr(response, "\"runs\":[") != NULL);
    assert(strstr(response, "tdoa_gauss_newton") != NULL);
    assert(strstr(response, "semblance") != NULL);
    assert(strstr(response, "\"solved\":true") != NULL);
    assert(strstr(response, "\"solved\":false") != NULL);
    assert(strstr(response, "no coherent candidate") != NULL);
}

static void test_runs_listing_requires_both_query_parameters(void)
{
    reset_fakes();
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "400 Bad Request") == 0);
    assert(s_list_calls == 0U);
}

static void test_runs_listing_surfaces_not_found_as_404(void)
{
    reset_fakes();
    s_list_result = ESP_ERR_NOT_FOUND;
    char response[512] = {0};
    httpd_req_t request = make_request("object_id=OBJ_0001&session_id=S000001", response, sizeof(response));

    assert(web_localization_runs_handler(&request) == ESP_OK);
    assert(request.response_status != NULL);
    assert(strcmp(request.response_status, "404 Not Found") == 0);
    assert(s_list_calls == 1U);
}

int main(void)
{
    test_runs_listing_round_trips_persisted_records();
    test_runs_listing_requires_both_query_parameters();
    test_runs_listing_surfaces_not_found_as_404();
    return 0;
}
