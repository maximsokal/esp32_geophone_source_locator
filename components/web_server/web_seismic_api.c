#include "web_server_internal.h"

#include <inttypes.h>
#include <stdlib.h>

#include "seismic_buffer.h"


#define WEB_SEISMIC_DEFAULT_POINTS  240
#define WEB_SEISMIC_MIN_POINTS      16
#define WEB_SEISMIC_MAX_POINTS      1200


static size_t web_seismic_requested_points(
    httpd_req_t *request
)
{
    size_t query_length =
        httpd_req_get_url_query_len(
            request
        );

    if (
        query_length == 0 ||
        query_length >= 64
    ) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    char query[64];

    if (
        httpd_req_get_url_query_str(
            request,
            query,
            sizeof(query)
        ) != ESP_OK
    ) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    char value[16];

    if (
        httpd_query_key_value(
            query,
            "points",
            value,
            sizeof(value)
        ) != ESP_OK
    ) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    char *end = NULL;

    long requested = strtol(value, &end, 10);

    if (
        end == value ||
        *end != '\0'
    ) {
        return WEB_SEISMIC_DEFAULT_POINTS;
    }

    if (requested < WEB_SEISMIC_MIN_POINTS) {
        requested = WEB_SEISMIC_MIN_POINTS;
    }

    if (requested > WEB_SEISMIC_MAX_POINTS) {
        requested = WEB_SEISMIC_MAX_POINTS;
    }

    return (size_t)requested;
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


esp_err_t web_seismic_api_register(
    httpd_handle_t server
)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uri = {
        .uri = "/api/seismic",
        .method = HTTP_GET,
        .handler = web_seismic_handler,
        .user_ctx = NULL,
    };

    return httpd_register_uri_handler(server, &uri);
}
