#include "seismic_recorder_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>


static bool seismic_recorder_waveform_add_overflows(
    uint64_t left,
    uint64_t right,
    uint64_t *result
)
{
    if (right > UINT64_MAX - left) {
        return true;
    }

    *result = left + right;
    return false;
}


static bool seismic_recorder_waveform_mul_overflows(
    uint64_t left,
    uint64_t right,
    uint64_t *result
)
{
    if (left != 0U && right > UINT64_MAX / left) {
        return true;
    }

    *result = left * right;
    return false;
}


static bool seismic_recorder_waveform_header_valid(
    const seismic_recorder_binary_header_t *header
)
{
    return
        header != NULL &&
        memcmp(header->magic, "GEOSEIS1", sizeof(header->magic)) == 0 &&
        header->version == SEISMIC_RECORDER_BINARY_VERSION &&
        header->channel_count == SEISMIC_CHANNEL_COUNT &&
        header->sample_rate_hz > 0U &&
        header->frame_size_bytes == sizeof(seismic_frame_t) &&
        header->reserved == 0U;
}


static void seismic_recorder_waveform_point_reset(
    seismic_recorder_waveform_point_t *point
)
{
    point->timestamp_us = 0;
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        point->min_uv[channel] = FLT_MAX;
        point->max_uv[channel] = -FLT_MAX;
    }
}


esp_err_t seismic_recorder_waveform_read_file(
    FILE *samples_file,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
)
{
    if (
        samples_file == NULL ||
        summary == NULL ||
        info == NULL ||
        points == NULL ||
        max_points == 0U ||
        max_points > SEISMIC_RECORDER_MAX_WAVEFORM_POINTS ||
        point_capacity < max_points
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    if (file_size < sizeof(seismic_recorder_binary_header_t)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (file_size > (uint64_t)LONG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(samples_file, 0L, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    seismic_recorder_binary_header_t header;
    if (fread(&header, sizeof(header), 1U, samples_file) != 1U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!seismic_recorder_waveform_header_valid(&header)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t payload_size =
        file_size - sizeof(seismic_recorder_binary_header_t);
    if (payload_size % sizeof(seismic_frame_t) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t source_frame_count =
        payload_size / sizeof(seismic_frame_t);

    if (start_frame > source_frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (frame_count == 0U) {
        frame_count = source_frame_count - start_frame;
    }

    if (frame_count > source_frame_count - start_frame) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t point_count =
        frame_count < (uint64_t)max_points
            ? (size_t)frame_count
            : max_points;

    uint64_t range_offset_frames = 0U;
    uint64_t range_offset_bytes = 0U;
    if (
        seismic_recorder_waveform_mul_overflows(
            start_frame,
            sizeof(seismic_frame_t),
            &range_offset_frames
        ) ||
        seismic_recorder_waveform_add_overflows(
            sizeof(seismic_recorder_binary_header_t),
            range_offset_frames,
            &range_offset_bytes
        ) ||
        range_offset_bytes > (uint64_t)LONG_MAX
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (
        point_count > 0U &&
        frame_count > UINT64_MAX / (uint64_t)point_count
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (
        fseek(
            samples_file,
            (long)range_offset_bytes,
            SEEK_SET
        ) != 0
    ) {
        return ESP_FAIL;
    }

    for (size_t point = 0; point < point_count; ++point) {
        seismic_recorder_waveform_point_reset(&points[point]);
    }

    int64_t first_timestamp_us = 0;
    int64_t last_timestamp_us = 0;
    bool have_timestamp = false;

    for (uint64_t frame_index = 0U; frame_index < frame_count; ++frame_index) {
        seismic_frame_t frame;
        if (fread(&frame, sizeof(frame), 1U, samples_file) != 1U) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (!have_timestamp) {
            first_timestamp_us = frame.timestamp_us;
            have_timestamp = true;
        }
        last_timestamp_us = frame.timestamp_us;

        for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (!isfinite(frame.channel_uv[channel])) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }

        /* Multiplication is safe because max_points is at most 1200. */
        const size_t point_index =
            point_count == 0U
                ? 0U
                : (size_t)((frame_index * (uint64_t)point_count) / frame_count);
        seismic_recorder_waveform_point_t *point = &points[point_index];

        if (point->min_uv[0] == FLT_MAX) {
            point->timestamp_us = frame.timestamp_us;
        }

        for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (frame.channel_uv[channel] < point->min_uv[channel]) {
                point->min_uv[channel] = frame.channel_uv[channel];
            }
            if (frame.channel_uv[channel] > point->max_uv[channel]) {
                point->max_uv[channel] = frame.channel_uv[channel];
            }
        }
    }

    info->complete = summary->complete;
    snprintf(info->object_id, sizeof(info->object_id), "%s", "");
    snprintf(info->session_id, sizeof(info->session_id), "%s", summary->session_id);
    snprintf(info->source_name, sizeof(info->source_name), "%s", summary->source_name);
    info->sample_rate_hz = header.sample_rate_hz;
    info->channel_count = (uint8_t)header.channel_count;
    info->samples_written = summary->samples_written;
    info->dropped_frames = summary->dropped_frames;
    info->recording_ms = summary->recording_ms;
    info->source_frame_count = source_frame_count;
    info->start_frame = start_frame;
    info->frame_count = frame_count;
    info->point_count = point_count;

    if (have_timestamp) {
        info->start_timestamp_us = first_timestamp_us;
        info->end_timestamp_us = last_timestamp_us;
    }

    return ESP_OK;
}
