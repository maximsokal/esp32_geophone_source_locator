#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "seismic_buffer.h"
#include "seismic_recorder.h"
#include "web_server_internal.h"

static httpd_uri_t s_live_uri;
static size_t s_heap_caps_calls;
static size_t s_heap_caps_max_size;
static size_t s_heap_caps_last_size;
static unsigned s_heap_caps_last_flags;


void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    ++s_heap_caps_calls;
    if (size > s_heap_caps_max_size) s_heap_caps_max_size = size;
    s_heap_caps_last_size = size;
    s_heap_caps_last_flags = capabilities;
    return malloc(size);
}


void heap_caps_free(void *pointer)
{
    free(pointer);
}


static void reset_heap_caps_stats(void)
{
    s_heap_caps_calls = 0U;
    s_heap_caps_max_size = 0U;
    s_heap_caps_last_size = 0U;
    s_heap_caps_last_flags = 0U;
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


static void push_synthetic_frames(uint32_t sample_rate_hz, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        float channels[SEISMIC_CHANNEL_COUNT] = {
            (float)((int)(index % 2001U) - 1000) * 0.25f,
            (float)((int)(index % 997U) - 498) * 0.5f,
            (float)((int)(index % 521U) - 260) * 1.25f,
            (float)((int)(index % 251U) - 125) * 2.0f,
        };
        if (index % 257U == 3U) channels[0] = -12345.125f;
        if (index % 263U == 7U) channels[0] = 19000.25f;
        const int64_t timestamp_us =
            (int64_t)(((uint64_t)(index + 1U) * 1000000U) / sample_rate_hz);
        assert(seismic_buffer_push(timestamp_us, channels) == ESP_OK);
    }
}


static void write_legacy_response(
    httpd_req_t *request,
    uint32_t requested_span_ms,
    size_t requested_points,
    size_t legacy_frame_count
)
{
    seismic_frame_t latest_frame;
    size_t latest_count = 0U;
    seismic_buffer_info_t info;
    assert(seismic_buffer_copy_latest(&latest_frame, 1U, &latest_count, &info) == ESP_OK);

    const uint64_t requested_frames = legacy_frame_count > 0U
        ? legacy_frame_count
        : info.sample_rate_hz == 0U
        ? 0U
        : ((uint64_t)info.sample_rate_hz * requested_span_ms + 999U) / 1000U;
    if (legacy_frame_count > 0U && info.sample_rate_hz > 0U) {
        const uint64_t legacy_span_ms =
            ((uint64_t)legacy_frame_count * 1000U + info.sample_rate_hz / 2U) / info.sample_rate_hz;
        requested_span_ms = (uint32_t)(legacy_span_ms == 0U ? 1U : legacy_span_ms);
    }
    const size_t frame_capacity = requested_frames < info.buffered_frames
        ? (size_t)requested_frames
        : info.buffered_frames;
    seismic_frame_t *frames = frame_capacity == 0U
        ? NULL
        : malloc(frame_capacity * sizeof(*frames));
    assert(frame_capacity == 0U || frames != NULL);
    size_t frame_count = 0U;
    if (frame_capacity > 0U) {
        assert(seismic_buffer_copy_latest(frames, frame_capacity, &frame_count, &info) == ESP_OK);
    }

    const size_t point_count = frame_count < requested_points ? frame_count : requested_points;
    const uint32_t decimation_factor = point_count == 0U
        ? 0U
        : (uint32_t)((frame_count + point_count - 1U) / point_count);
    const uint64_t effective_span_ms = info.sample_rate_hz == 0U
        ? 0U
        : ((uint64_t)frame_count * 1000U + info.sample_rate_hz / 2U) / info.sample_rate_hz;

    assert(httpd_resp_set_type(request, "application/json; charset=utf-8") == ESP_OK);
    assert(httpd_resp_set_hdr(request, "Cache-Control", "no-store") == ESP_OK);
    web_json_writer_t writer;
    web_json_writer_init(&writer, request);
    web_json_writer_printf(
        &writer,
        "{\"source\":\"%s\",\"sample_rate_hz\":%" PRIu32 ",",
        seismic_buffer_source_name(info.source),
        info.sample_rate_hz
    );
    web_json_writer_printf(
        &writer,
        "\"count\":%u,\"frame_count\":%u,\"requested_span_ms\":%" PRIu32 ",",
        (unsigned)point_count,
        (unsigned)frame_count,
        requested_span_ms
    );
    web_json_writer_printf(
        &writer,
        "\"span_ms\":%" PRIu64 ",\"decimation_factor\":%" PRIu32 ",",
        effective_span_ms,
        decimation_factor
    );
    web_json_writer_printf(
        &writer,
        "\"buffered_frames\":%u,\"total_frames\":%" PRIu64 ",\"points\":[",
        (unsigned)info.buffered_frames,
        info.total_frames
    );

    for (size_t point = 0U; point < point_count; ++point) {
        const size_t start = (point * frame_count) / point_count;
        size_t end = ((point + 1U) * frame_count) / point_count;
        if (end <= start) end = start + 1U;
        if (point > 0U) web_json_writer_write(&writer, ",");
        web_json_writer_printf(
            &writer,
            "{\"timestamp_us\":%" PRId64 ",\"end_timestamp_us\":%" PRId64 ",\"channels\":[",
            frames[start].timestamp_us,
            frames[end - 1U].timestamp_us
        );
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            float minimum = frames[start].channel_uv[channel];
            float maximum = minimum;
            for (size_t frame = start + 1U; frame < end; ++frame) {
                const float value = frames[frame].channel_uv[channel];
                if (value < minimum) minimum = value;
                if (value > maximum) maximum = value;
            }
            if (channel > 0U) web_json_writer_write(&writer, ",");
            web_json_writer_printf(
                &writer,
                "{\"id\":%u,\"min_uv\":%.3f,\"max_uv\":%.3f}",
                (unsigned)(channel + 1U),
                minimum,
                maximum
            );
        }
        web_json_writer_write(&writer, "]}");
    }
    web_json_writer_write(&writer, "]}");
    assert(web_json_writer_finish(&writer) == ESP_OK);
    free(frames);
}


static void assert_live_response_matches_legacy(
    const char *query,
    uint32_t requested_span_ms,
    size_t requested_points,
    size_t legacy_frame_count,
    size_t expected_frame_capacity
)
{
    char expected[16384] = { 0 };
    char actual[16384] = { 0 };
    httpd_req_t expected_request = {
        .uri = "/api/seismic",
        .response_body = expected,
        .response_capacity = sizeof(expected),
    };
    write_legacy_response(
        &expected_request,
        requested_span_ms,
        requested_points,
        legacy_frame_count
    );

    httpd_req_t request = {
        .uri = "/api/seismic",
        .query = query,
        .response_body = actual,
        .response_capacity = sizeof(actual),
    };
    reset_heap_caps_stats();
    assert(s_live_uri.handler != NULL);
    assert(s_live_uri.handler(&request) == ESP_OK);
    assert(request.error_status == 0U);
    assert(request.content_type != NULL && strstr(request.content_type, "application/json") != NULL);
    assert(request.cache_control != NULL && strcmp(request.cache_control, "no-store") == 0);
    assert(request.response_length == expected_request.response_length);
    assert(strcmp(actual, expected) == 0);

    const size_t expected_scratch_frames = expected_frame_capacity < 1024U
        ? expected_frame_capacity
        : 1024U;
    assert(s_heap_caps_calls == (expected_scratch_frames == 0U ? 0U : 1U));
    assert(s_heap_caps_max_size == expected_scratch_frames * sizeof(seismic_frame_t));
    assert(s_heap_caps_last_size == expected_scratch_frames * sizeof(seismic_frame_t));
    assert(s_heap_caps_max_size <= 24U * 1024U);
    if (expected_scratch_frames > 0U) {
        assert((s_heap_caps_last_flags & (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) ==
            (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
}


static void test_chunked_output_and_allocation_at_supported_rates(void)
{
    static const uint32_t rates[] = { 500U, 20000U, 40000U };
    for (size_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        const uint32_t rate = rates[i];
        assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, rate) == ESP_OK);
        const size_t capacity = seismic_buffer_capacity();
        assert(capacity >= 2048U);
        push_synthetic_frames(rate, capacity);

        assert_live_response_matches_legacy(
            "span_ms=1&max_points=1", 1U, 16U, 0U,
            rate / 1000U > 1U ? rate / 1000U : 1U
        );
        assert_live_response_matches_legacy(
            "span_ms=60000&max_points=16", 60000U, 16U, 0U, capacity
        );
        assert_live_response_matches_legacy(
            "points=16", 480U, 16U, 16U, 16U
        );
    }
}


int main(void)
{
    assert(seismic_buffer_init() == ESP_OK);
    assert(web_seismic_api_register((httpd_handle_t)(uintptr_t)1U) == ESP_OK);
    test_chunked_output_and_allocation_at_supported_rates();
    puts("web seismic chunked time-span API tests passed");
    return 0;
}
