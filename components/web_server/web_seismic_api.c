#include "web_server_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "seismic_buffer.h"
#include "seismic_envelope.h"
#include "seismic_recorder.h"


#define WEB_SEISMIC_DEFAULT_POINTS  240
#define WEB_SEISMIC_DEFAULT_SPAN_MS 480U
#define WEB_SEISMIC_MAX_SPAN_MS    60000U
#define WEB_SEISMIC_MIN_POINTS      16
#define WEB_SEISMIC_MAX_POINTS      1200
#define WEB_SEISMIC_CHUNK_FRAMES    1024U


static esp_err_t web_seismic_query_copy(
    httpd_req_t *request,
    char *query,
    size_t query_size
)
{
    const size_t query_length =
        httpd_req_get_url_query_len(
            request
        );

    if (
        query_length == 0 ||
        query_length >= query_size
    ) {
        return query_length == 0
            ? ESP_ERR_NOT_FOUND
            : ESP_ERR_INVALID_SIZE;
    }

    return httpd_req_get_url_query_str(request, query, query_size);
}


static esp_err_t web_seismic_query_value(
    const char *query,
    const char *key,
    char *value,
    size_t value_size
)
{
    return httpd_query_key_value(query, key, value, value_size);
}


static bool web_seismic_parse_u64(
    const char *text,
    uint64_t *value
)
{
    if (text == NULL || value == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0') {
        return false;
    }

    *value = (uint64_t)parsed;
    return true;
}


static bool web_seismic_parse_points(
    const char *text,
    size_t *points
)
{
    uint64_t requested = 0U;
    if (!web_seismic_parse_u64(text, &requested)) {
        return false;
    }

    if (requested < WEB_SEISMIC_MIN_POINTS) {
        requested = WEB_SEISMIC_MIN_POINTS;
    }

    if (requested > WEB_SEISMIC_MAX_POINTS) {
        requested = WEB_SEISMIC_MAX_POINTS;
    }

    *points = (size_t)requested;
    return true;
}


static esp_err_t web_seismic_live_request_options(
    httpd_req_t *request,
    uint32_t *span_ms,
    size_t *points,
    size_t *legacy_frame_count
)
{
    *span_ms = WEB_SEISMIC_DEFAULT_SPAN_MS;
    *points = WEB_SEISMIC_DEFAULT_POINTS;
    *legacy_frame_count = 0U;

    char query[256];
    const esp_err_t query_err = web_seismic_query_copy(request, query, sizeof(query));
    if (query_err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (query_err != ESP_OK) return ESP_ERR_INVALID_ARG;

    char value[24];
    const esp_err_t span_err = web_seismic_query_value(query, "span_ms", value, sizeof(value));
    if (span_err == ESP_OK) {
        uint64_t requested = 0U;
        if (!web_seismic_parse_u64(value, &requested) || requested == 0U ||
            requested > WEB_SEISMIC_MAX_SPAN_MS) {
            return ESP_ERR_INVALID_ARG;
        }
        *span_ms = (uint32_t)requested;
    }
    else if (span_err != ESP_ERR_NOT_FOUND) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t max_points_err =
        web_seismic_query_value(query, "max_points", value, sizeof(value));
    if (max_points_err == ESP_OK) {
        return web_seismic_parse_points(value, points) ? ESP_OK : ESP_ERR_INVALID_ARG;
    }
    if (max_points_err != ESP_ERR_NOT_FOUND) return ESP_ERR_INVALID_ARG;

    /* `points` remains an alias so existing live-chart clients keep working. */
    if (web_seismic_query_value(query, "points", value, sizeof(value)) == ESP_OK) {
        size_t legacy_points = WEB_SEISMIC_DEFAULT_POINTS;
        if (web_seismic_parse_points(value, &legacy_points)) {
            *points = legacy_points;
            if (span_err == ESP_ERR_NOT_FOUND) *legacy_frame_count = legacy_points;
        }
    }
    return ESP_OK;
}


static esp_err_t web_seismic_saved_request_options(
    httpd_req_t *request,
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE],
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    uint64_t *start_frame,
    uint64_t *frame_count,
    size_t *points
)
{
    char query[256];
    esp_err_t err = web_seismic_query_copy(request, query, sizeof(query));
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    char value[32];
    if (
        web_seismic_query_value(
            query,
            "object_id",
            object_id,
            SEISMIC_RECORDER_OBJECT_ID_SIZE
        ) != ESP_OK ||
        web_seismic_query_value(
            query,
            "session_id",
            session_id,
            SEISMIC_RECORDER_SESSION_ID_SIZE
        ) != ESP_OK
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *start_frame = 0U;
    *frame_count = 0U;
    *points = WEB_SEISMIC_DEFAULT_POINTS;

    if (web_seismic_query_value(query, "start_frame", value, sizeof(value)) == ESP_OK) {
        if (!web_seismic_parse_u64(value, start_frame)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (web_seismic_query_value(query, "frame_count", value, sizeof(value)) == ESP_OK) {
        if (!web_seismic_parse_u64(value, frame_count)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (web_seismic_query_value(query, "points", value, sizeof(value)) == ESP_OK) {
        if (!web_seismic_parse_points(value, points)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}


static esp_err_t web_seismic_handler(
    httpd_req_t *request
)
{
    uint32_t requested_span_ms = 0U;
    size_t requested_points = 0U;
    size_t legacy_frame_count = 0U;
    if (web_seismic_live_request_options(
            request,
            &requested_span_ms,
            &requested_points,
            &legacy_frame_count) != ESP_OK) {
        return httpd_resp_send_err(request, 400, "span_ms and max_points must be valid numeric values");
    }

    seismic_frame_t latest_frame;
    size_t latest_count = 0U;
    seismic_buffer_info_t info;
    esp_err_t err = seismic_buffer_copy_latest(&latest_frame, 1U, &latest_count, &info);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Seismic buffer unavailable");
    }

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
    const size_t chunk_capacity = frame_capacity < WEB_SEISMIC_CHUNK_FRAMES
        ? frame_capacity
        : WEB_SEISMIC_CHUNK_FRAMES;
    seismic_frame_t *chunk = chunk_capacity == 0U
        ? NULL
        : heap_caps_malloc(chunk_capacity * sizeof(*chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk_capacity > 0U && chunk == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    const size_t frame_count = frame_capacity;
    const size_t point_count = frame_count < requested_points ? frame_count : requested_points;
    const uint32_t decimation_factor = point_count == 0U
        ? 0U
        : (uint32_t)((frame_count + point_count - 1U) / point_count);
    const uint64_t effective_span_ms = info.sample_rate_hz == 0U
        ? 0U
        : ((uint64_t)frame_count * 1000U + info.sample_rate_hz / 2U) / info.sample_rate_hz;

    httpd_resp_set_type(
        request,
        "application/json; charset=utf-8"
    );

    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

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

    uint64_t cursor = info.total_frames - frame_count;
    size_t frames_copied = 0U;
    size_t chunk_count = 0U;
    size_t chunk_offset = 0U;
    for (size_t point = 0U; point < point_count; ++point) {
        const size_t start = (point * frame_count) / point_count;
        size_t end = ((point + 1U) * frame_count) / point_count;
        if (end <= start) end = start + 1U;
        const size_t point_frame_count = end - start;
        float bucket_min[SEISMIC_CHANNEL_COUNT];
        float bucket_max[SEISMIC_CHANNEL_COUNT];
        bool bucket_initialized[1];
        seismic_envelope_accumulator_t reducer;
        if (!seismic_envelope_accumulator_begin(
                &reducer,
                point_frame_count,
                1U,
                1U,
                bucket_min,
                bucket_max,
                bucket_initialized
            )) {
            heap_caps_free(chunk);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Seismic buffer unavailable");
        }

        int64_t timestamp_us = 0;
        int64_t end_timestamp_us = 0;
        size_t point_frames_copied = 0U;
        while (point_frames_copied < point_frame_count) {
            if (chunk_offset == chunk_count) {
                const size_t remaining = frame_count - frames_copied;
                const size_t max_frames = remaining < chunk_capacity
                    ? remaining
                    : chunk_capacity;
                uint64_t dropped = 0U;
                seismic_buffer_info_t chunk_info;
                err = seismic_buffer_copy_since(
                    &cursor,
                    chunk,
                    max_frames,
                    &chunk_count,
                    &dropped,
                    &chunk_info
                );
                if (err != ESP_OK || dropped != 0U || chunk_count == 0U) {
                    heap_caps_free(chunk);
                    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Seismic buffer unavailable");
                }
                frames_copied += chunk_count;
                chunk_offset = 0U;
            }

            const size_t point_remaining = point_frame_count - point_frames_copied;
            const size_t chunk_remaining = chunk_count - chunk_offset;
            const size_t take = point_remaining < chunk_remaining
                ? point_remaining
                : chunk_remaining;
            if (point_frames_copied == 0U) {
                timestamp_us = chunk[chunk_offset].timestamp_us;
            }
            end_timestamp_us = chunk[chunk_offset + take - 1U].timestamp_us;
            if (!seismic_envelope_accumulator_add(
                    &reducer,
                    point_frames_copied,
                    chunk + chunk_offset,
                    take
                )) {
                heap_caps_free(chunk);
                return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Seismic buffer unavailable");
            }
            point_frames_copied += take;
            chunk_offset += take;
        }
        if (!seismic_envelope_accumulator_finish(&reducer, false)) {
            heap_caps_free(chunk);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Seismic buffer unavailable");
        }

        if (point > 0U) web_json_writer_write(&writer, ",");
        web_json_writer_printf(
            &writer,
            "{\"timestamp_us\":%" PRId64 ",\"end_timestamp_us\":%" PRId64 ",\"channels\":[",
            timestamp_us,
            end_timestamp_us
        );

        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (channel > 0U) web_json_writer_write(&writer, ",");
            web_json_writer_printf(
                &writer,
                "{\"id\":%u,\"min_uv\":%.3f,\"max_uv\":%.3f}",
                (unsigned)(channel + 1U),
                bucket_min[channel],
                bucket_max[channel]
            );
        }
        web_json_writer_write(&writer, "]}");
    }

    web_json_writer_write(&writer, "]}");

    heap_caps_free(chunk);

    return web_json_writer_finish(&writer);
}


static int web_seismic_saved_http_status(
    esp_err_t err
)
{
    switch (err) {
        case ESP_ERR_NOT_FOUND:
            return 404;

        case ESP_ERR_INVALID_STATE:
            return 409;

        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
        case ESP_ERR_INVALID_RESPONSE:
            return 422;

        case ESP_ERR_NO_MEM:
            return 500;

        case ESP_OK:
        default:
            return 500;
    }
}


static esp_err_t web_seismic_saved_handler(
    httpd_req_t *request
)
{
    char object_id[SEISMIC_RECORDER_OBJECT_ID_SIZE] = {0};
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    uint64_t start_frame = 0U;
    uint64_t frame_count = 0U;
    size_t requested_points = WEB_SEISMIC_DEFAULT_POINTS;

    esp_err_t err = web_seismic_saved_request_options(
        request,
        object_id,
        session_id,
        &start_frame,
        &frame_count,
        &requested_points
    );
    if (err != ESP_OK) {
        return httpd_resp_send_err(
            request,
            400,
            "object_id, session_id and numeric range parameters are required"
        );
    }

    seismic_recorder_waveform_point_t *points = calloc(
        requested_points,
        sizeof(*points)
    );
    if (points == NULL) {
        return httpd_resp_send_err(
            request,
            500,
            "Out of memory"
        );
    }

    seismic_recorder_waveform_info_t info;
    err = seismic_recorder_read_session(
        object_id,
        session_id,
        start_frame,
        frame_count,
        requested_points,
        points,
        requested_points,
        &info
    );
    if (err != ESP_OK) {
        free(points);
        return httpd_resp_send_err(
            request,
            web_seismic_saved_http_status(err),
            err == ESP_ERR_INVALID_STATE
                ? "Session is not readable"
                : "Saved seismic session is unavailable or invalid"
        );
    }

    httpd_resp_set_type(
        request,
        "application/json; charset=utf-8"
    );
    httpd_resp_set_hdr(
        request,
        "Cache-Control",
        "no-store"
    );

    web_json_writer_t writer;
    web_json_writer_init(&writer, request);
    web_json_writer_write(&writer, "{");
    web_json_writer_printf(
        &writer,
        "\"object_id\":\"%s\",\"session_id\":\"%s\",",
        info.object_id,
        info.session_id
    );
    web_json_writer_printf(
        &writer,
        "\"source\":\"%s\","
        "\"status\":\"%s\","
        "\"complete\":%s,"
        "\"readable\":%s,"
        "\"recovered\":%s,"
        "\"sample_rate_hz\":%" PRIu32 ",",
        info.source_name,
        seismic_recorder_session_status_name(info.status),
        info.complete ? "true" : "false",
        info.readable ? "true" : "false",
        info.recovered ? "true" : "false",
        info.sample_rate_hz
    );
    web_json_writer_printf(
        &writer,
        "\"channel_count\":%u,"
        "\"samples_written\":%" PRIu64 ","
        "\"dropped_frames_available\":%s,"
        "\"dropped_frames\":%" PRIu64 ",",
        (unsigned)info.channel_count,
        info.samples_written,
        info.dropped_frames_available ? "true" : "false",
        info.dropped_frames
    );
    web_json_writer_printf(
        &writer,
        "\"recording_ms_available\":%s,"
        "\"recording_ms\":%" PRIu64 ","
        "\"source_frame_count\":%" PRIu64 ","
        "\"start_frame\":%" PRIu64 ",",
        info.recording_ms_available ? "true" : "false",
        info.recording_ms,
        info.source_frame_count,
        info.start_frame
    );
    web_json_writer_printf(
        &writer,
        "\"frame_count\":%" PRIu64 ","
        "\"count\":%u,"
        "\"start_timestamp_us\":%" PRId64 ",",
        info.frame_count,
        (unsigned)info.point_count,
        info.start_timestamp_us
    );
    web_json_writer_printf(
        &writer,
        "\"end_timestamp_us\":%" PRId64 ","
        "\"cadence\":{"
        "\"available\":%s,"
        "\"timer_ticks\":%" PRIu64 ","
        "\"conversion_attempts\":%" PRIu64 ","
        "\"collapsed_periods\":%" PRIu64
        "},\"points\":[",
        info.end_timestamp_us,
        info.cadence.available ? "true" : "false",
        info.cadence.timer_ticks,
        info.cadence.conversion_attempts,
        info.cadence.collapsed_periods
    );

    for (size_t point_index = 0U; point_index < info.point_count; ++point_index) {
        if (point_index > 0U) {
            web_json_writer_write(&writer, ",");
        }

        web_json_writer_printf(
            &writer,
            "{\"timestamp_us\":%" PRId64 ",\"end_timestamp_us\":%" PRId64 ",\"channels\":[",
            points[point_index].timestamp_us,
            points[point_index].end_timestamp_us
        );

        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (channel > 0U) {
                web_json_writer_write(&writer, ",");
            }
            web_json_writer_printf(
                &writer,
                "{\"id\":%u,\"min_uv\":%.3f,\"max_uv\":%.3f}",
                (unsigned)(channel + 1U),
                points[point_index].min_uv[channel],
                points[point_index].max_uv[channel]
            );
        }

        web_json_writer_write(&writer, "]}");
    }

    web_json_writer_write(&writer, "]}");
    free(points);
    return web_json_writer_finish(&writer);
}


esp_err_t web_seismic_api_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t saved_uri = {
        .uri = "/api/seismic/session",
        .method = HTTP_GET,
        .handler = web_seismic_saved_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = web_server_register_uri_handler(server, &saved_uri);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uri = {
        .uri = "/api/seismic",
        .method = HTTP_GET,
        .handler = web_seismic_handler,
        .user_ctx = NULL,
    };

    return web_server_register_uri_handler(server, &uri);
}
