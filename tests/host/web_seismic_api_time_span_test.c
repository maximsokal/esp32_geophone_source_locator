#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "web_server_internal.h"

static httpd_uri_t s_live_uri;

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
    if (query == NULL || key == NULL || value == NULL || value_size == 0U) return ESP_ERR_INVALID_ARG;
    const size_t key_length = strlen(key);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        const char *equals = strchr(cursor, '=');
        if (equals != NULL && (end == NULL || equals < end) &&
            (size_t)(equals - cursor) == key_length && strncmp(cursor, key, key_length) == 0) {
            const char *start = equals + 1;
            const size_t length = end == NULL ? strlen(start) : (size_t)(end - start);
            if (length == 0U || length >= value_size) return ESP_ERR_INVALID_SIZE;
            memcpy(value, start, length);
            value[length] = '\0';
            return ESP_OK;
        }
        if (end == NULL) break;
        cursor = end + 1;
    }
    return ESP_ERR_NOT_FOUND;
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

esp_err_t httpd_resp_send_chunk(httpd_req_t *request, const char *buffer, ssize_t length)
{
    if (buffer == NULL && length == 0) return ESP_OK;
    assert(request != NULL && buffer != NULL && length > 0);
    assert(request->response_length + (size_t)length < request->response_capacity);
    memcpy(request->response_body + request->response_length, buffer, (size_t)length);
    request->response_length += (size_t)length;
    request->response_body[request->response_length] = '\0';
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int status, const char *message)
{
    (void)message;
    if (request != NULL) request->error_status = (unsigned)status;
    return ESP_FAIL;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *uri)
{
    (void)server;
    if (strcmp(uri->uri, "/api/seismic") == 0) s_live_uri = *uri;
    return ESP_OK;
}

esp_err_t web_server_route_tracking_register(const char *uri, int method)
{
    (void)uri;
    (void)method;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session(
    const char *object_id,
    const char *session_id,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
)
{
    (void)object_id;
    (void)session_id;
    (void)start_frame;
    (void)frame_count;
    (void)max_points;
    (void)points;
    (void)point_capacity;
    (void)info;
    return ESP_ERR_NOT_FOUND;
}

static void push_frames(uint32_t sample_rate_hz, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        float channels[SEISMIC_CHANNEL_COUNT] = {
            (float)(index + 1U), (float)(index + 2U), (float)(index + 3U), (float)(index + 4U),
        };
        if (index == 6U) {
            channels[0] = 18000.0f;
            channels[1] = -12000.0f;
        }
        const int64_t timestamp_us =
            (int64_t)(((uint64_t)(index + 1U) * 1000000U) / sample_rate_hz);
        assert(seismic_buffer_push(timestamp_us, channels) == ESP_OK);
    }
}

static void assert_live_response(const char *query, char *response, size_t response_capacity)
{
    httpd_req_t request = {
        .uri = "/api/seismic",
        .query = query,
        .response_body = response,
        .response_capacity = response_capacity,
    };
    assert(s_live_uri.handler != NULL);
    assert(s_live_uri.handler(&request) == ESP_OK);
    assert(request.error_status == 0U);
    assert(request.content_type != NULL && strstr(request.content_type, "application/json") != NULL);
    assert(request.cache_control != NULL && strcmp(request.cache_control, "no-store") == 0);
    assert(response[0] == '{' && strstr(response, "\"points\":[") != NULL);
    assert(request.response_length > 0U && response[request.response_length - 1U] == '}');
}

static void test_live_api_time_span_and_envelope_at_high_rates(void)
{
    static const uint32_t rates[] = { 20000U, 40000U };
    static const size_t frame_counts[] = { 400U, 800U };
    for (size_t index = 0U; index < sizeof(rates) / sizeof(rates[0]); ++index) {
        const uint32_t rate = rates[index];
        const size_t frames = frame_counts[index];
        assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, rate) == ESP_OK);
        push_frames(rate, frames);

        char response[8192] = { 0 };
        assert_live_response("span_ms=20&max_points=16", response, sizeof(response));
        char expected[64];
        (void)snprintf(expected, sizeof(expected), "\"sample_rate_hz\":%u", rate);
        assert(strstr(response, expected) != NULL);
        (void)snprintf(expected, sizeof(expected), "\"frame_count\":%u", (unsigned)frames);
        assert(strstr(response, expected) != NULL);
        assert(strstr(response, "\"requested_span_ms\":20") != NULL);
        assert(strstr(response, "\"span_ms\":20") != NULL);
        assert(strstr(response, "\"count\":16") != NULL);
        (void)snprintf(expected, sizeof(expected), "\"decimation_factor\":%u", (unsigned)(frames / 16U));
        assert(strstr(response, expected) != NULL);
        assert(strstr(response, "\"min_uv\":1.000,\"max_uv\":18000.000") != NULL);
        assert(strstr(response, "\"min_uv\":-12000.000") != NULL);
    }
}

static void test_live_api_default_and_legacy_points_parameter(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);
    push_frames(500U, 20U);
    char response[8192] = { 0 };
    assert_live_response("points=16", response, sizeof(response));
    assert(strstr(response, "\"requested_span_ms\":32") != NULL);
    assert(strstr(response, "\"frame_count\":16") != NULL);
    assert(strstr(response, "\"count\":16") != NULL);
    assert(strstr(response, "\"timestamp_us\":10000") != NULL);
}

static void test_live_api_span_boundaries_and_bad_queries(void)
{
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 20000U) == ESP_OK);
    push_frames(20000U, 400U);
    char response[8192] = { 0 };
    assert_live_response("span_ms=1&max_points=1", response, sizeof(response));
    assert(strstr(response, "\"requested_span_ms\":1") != NULL);
    assert(strstr(response, "\"frame_count\":20") != NULL);
    assert(strstr(response, "\"count\":16") != NULL); /* point floor */

    memset(response, 0, sizeof(response));
    assert_live_response("span_ms=60000&max_points=16", response, sizeof(response));
    assert(strstr(response, "\"requested_span_ms\":60000") != NULL);
    assert(strstr(response, "\"frame_count\":400") != NULL);

    static const char *bad_queries[] = {
        "span_ms=0", "span_ms=", "span_ms=60001", "max_points=bad",
    };
    for (size_t index = 0U; index < sizeof(bad_queries) / sizeof(bad_queries[0]); ++index) {
        httpd_req_t request = { .uri = "/api/seismic", .query = bad_queries[index] };
        assert(s_live_uri.handler(&request) == ESP_FAIL);
        assert(request.error_status == 400U);
    }
}

int main(void)
{
    assert(seismic_buffer_init() == ESP_OK);
    assert(web_seismic_api_register((httpd_handle_t)(uintptr_t)1U) == ESP_OK);
    test_live_api_time_span_and_envelope_at_high_rates();
    test_live_api_default_and_legacy_points_parameter();
    test_live_api_span_boundaries_and_bad_queries();
    puts("web seismic time-span API tests passed");
    return 0;
}
