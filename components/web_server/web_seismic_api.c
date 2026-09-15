#include "web_server_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "seismic_buffer.h"
#include "seismic_recorder.h"


#define WEB_SEISMIC_DEFAULT_POINTS  240
#define WEB_SEISMIC_MIN_POINTS      16
#define WEB_SEISMIC_MAX_POINTS      1200


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


static size_t web_seismic_requested_points(
    httpd_req_t *request
)
{
    char query[256];
    if (web_seismic_query_copy(request, query, sizeof(query)) != ESP_OK) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    char value[24];
    if (web_seismic_query_value(query, "points", value, sizeof(value)) != ESP_OK) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    size_t points = WEB_SEISMIC_DEFAULT_POINTS;
    return web_seismic_parse_points(value, &points)
        ? points
        : WEB_SEISMIC_DEFAULT_POINTS;
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
    size_t requested_points =
        web_seismic_requested_points(
            request
        );

    seismic_frame_t *frames =
        calloc(
            requested_points,
            sizeof(seismic_frame_t)
        );

    if (frames == NULL) {
        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Out of memory"
        );
    }

    size_t frame_count = 0;
    seismic_buffer_info_t info;

    esp_err_t err =
        seismic_buffer_copy_latest(
            frames,
            requested_points,
            &frame_count,
            &info
        );

    if (err != ESP_OK) {
        free(frames);

        return httpd_resp_send_err(
            request,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Seismic buffer unavailable"
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

    web_json_writer_printf(
        &writer,
        "{"
        "\"source\":\"%s\","
        "\"sample_rate_hz\":%" PRIu32 ","
        "\"count\":%u,"
        "\"buffered_frames\":%u,"
        "\"total_frames\":%" PRIu64 ","
        "\"timestamps_ms\":[",
        seismic_buffer_source_name(info.source),
        info.sample_rate_hz,
        (unsigned)frame_count,
        (unsigned)info.buffered_frames,
        info.total_frames
    );

    for (size_t i = 0; i < frame_count; ++i) {
        if (i > 0) {
            web_json_writer_write(&writer, ",");
        }

        web_json_writer_printf(
            &writer,
            "%" PRId64,
            frames[i].timestamp_us / 1000
        );
    }

    web_json_writer_write(
        &writer,
        "],\"channels\":["
    );

    for (
        size_t channel = 0;
        channel < SEISMIC_CHANNEL_COUNT;
        ++channel
    ) {
        if (channel > 0) {
            web_json_writer_write(&writer, ",");
        }

        web_json_writer_printf(
            &writer,
            "{"
            "\"id\":%u,"
            "\"samples_uv\":[",
            (unsigned)(channel + 1)
        );

        for (size_t i = 0; i < frame_count; ++i) {
            if (i > 0) {
                web_json_writer_write(&writer, ",");
            }

            web_json_writer_printf(
                &writer,
                "%.3f",
                frames[i].channel_uv[channel]
            );
        }

        web_json_writer_write(&writer, "]}");
    }

    web_json_writer_write(&writer, "]}");

    free(frames);

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
