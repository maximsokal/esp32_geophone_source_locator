#include "seismic_recorder_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>


#define SEISMIC_RECORDER_FILE_WRAPPER_STAGING_FRAMES 64U


typedef struct
{
    FILE *file;

} seismic_recorder_waveform_file_context_t;


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


static esp_err_t seismic_recorder_waveform_validate_contract(
    uint64_t file_size,
    const seismic_recorder_binary_header_t *header,
    const seismic_recorder_session_summary_t *summary
)
{
    if (header == NULL || summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_recorder_waveform_header_valid(header)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (
        summary->sample_rate_hz == 0U ||
        summary->channel_count != SEISMIC_CHANNEL_COUNT ||
        header->sample_rate_hz != summary->sample_rate_hz ||
        header->channel_count != summary->channel_count
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t payload_size = 0U;
    uint64_t expected_file_size = 0U;
    if (
        seismic_recorder_waveform_mul_overflows(
            summary->samples_written,
            sizeof(seismic_frame_t),
            &payload_size
        ) ||
        seismic_recorder_waveform_add_overflows(
            sizeof(seismic_recorder_binary_header_t),
            payload_size,
            &expected_file_size
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return file_size == expected_file_size
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}


static void seismic_recorder_waveform_point_reset(
    seismic_recorder_waveform_point_t *point
)
{
    point->timestamp_us = 0;
    point->end_timestamp_us = 0;
    for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        point->min_uv[channel] = FLT_MAX;
        point->max_uv[channel] = -FLT_MAX;
    }
}


static esp_err_t seismic_recorder_waveform_file_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    seismic_recorder_waveform_file_context_t *file_context =
        (seismic_recorder_waveform_file_context_t *)context;

    if (
        file_context == NULL ||
        file_context->file == NULL ||
        buffer == NULL ||
        size_bytes == 0U ||
        offset_bytes > (uint64_t)LONG_MAX
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(file_context->file, (long)offset_bytes, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    return fread(buffer, 1U, size_bytes, file_context->file) == size_bytes
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}


esp_err_t seismic_recorder_waveform_read_source(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_frame_t *staging,
    size_t staging_capacity,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
)
{
    if (
        read_at == NULL ||
        summary == NULL ||
        info == NULL ||
        staging == NULL ||
        staging_capacity == 0U ||
        staging_capacity > SIZE_MAX / sizeof(seismic_frame_t) ||
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

    seismic_recorder_binary_header_t header;
    esp_err_t err = read_at(
        read_context,
        0U,
        &header,
        sizeof(header)
    );
    if (err != ESP_OK) {
        return err;
    }

    err = seismic_recorder_waveform_validate_contract(
        file_size,
        &header,
        summary
    );
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t source_frame_count = summary->samples_written;

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

    for (size_t point = 0; point < point_count; ++point) {
        seismic_recorder_waveform_point_reset(&points[point]);
    }

    int64_t first_timestamp_us = 0;
    int64_t last_timestamp_us = 0;
    bool have_timestamp = false;
    uint64_t processed = 0U;

    while (processed < frame_count) {
        const uint64_t remaining = frame_count - processed;
        const size_t chunk_count =
            remaining < (uint64_t)staging_capacity
                ? (size_t)remaining
                : staging_capacity;

        uint64_t chunk_frame_offset = 0U;
        uint64_t chunk_offset_bytes = 0U;
        const uint64_t absolute_frame = start_frame + processed;
        if (
            seismic_recorder_waveform_mul_overflows(
                absolute_frame,
                sizeof(seismic_frame_t),
                &chunk_frame_offset
            ) ||
            seismic_recorder_waveform_add_overflows(
                sizeof(seismic_recorder_binary_header_t),
                chunk_frame_offset,
                &chunk_offset_bytes
            ) ||
            chunk_offset_bytes > (uint64_t)LONG_MAX
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        const size_t chunk_bytes = chunk_count * sizeof(seismic_frame_t);
        err = read_at(
            read_context,
            chunk_offset_bytes,
            staging,
            chunk_bytes
        );
        if (err != ESP_OK) {
            return err;
        }

        /* read_at() owns the physical storage window. Reduction happens only
         * after that callback has returned, so CPU work never keeps SPI2/TFT
         * quiesced. */
        for (size_t chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
            const seismic_frame_t *frame = &staging[chunk_index];
            const uint64_t frame_index = processed + (uint64_t)chunk_index;

            if (have_timestamp && frame->timestamp_us < last_timestamp_us) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (!have_timestamp) {
                first_timestamp_us = frame->timestamp_us;
                have_timestamp = true;
            }
            last_timestamp_us = frame->timestamp_us;

            for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                if (!isfinite(frame->channel_uv[channel])) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            }

            /* The overflow bound above makes this multiplication safe. */
            const size_t point_index =
                point_count == 0U
                    ? 0U
                    : (size_t)((frame_index * (uint64_t)point_count) / frame_count);
            seismic_recorder_waveform_point_t *point = &points[point_index];

            if (point->min_uv[0] == FLT_MAX) {
                point->timestamp_us = frame->timestamp_us;
            }
            point->end_timestamp_us = frame->timestamp_us;

            for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                if (frame->channel_uv[channel] < point->min_uv[channel]) {
                    point->min_uv[channel] = frame->channel_uv[channel];
                }
                if (frame->channel_uv[channel] > point->max_uv[channel]) {
                    point->max_uv[channel] = frame->channel_uv[channel];
                }
            }
        }

        processed += (uint64_t)chunk_count;
    }

    info->complete = summary->complete;
    snprintf(info->object_id, sizeof(info->object_id), "%s", "");
    snprintf(info->session_id, sizeof(info->session_id), "%s", summary->session_id);
    snprintf(info->source_name, sizeof(info->source_name), "%s", summary->source_name);
    info->sample_rate_hz = summary->sample_rate_hz;
    info->channel_count = summary->channel_count;
    info->samples_written = summary->samples_written;
    info->dropped_frames = summary->dropped_frames;
    info->recording_ms = summary->recording_ms;
    info->cadence = summary->cadence;
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
    if (samples_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    seismic_frame_t staging[SEISMIC_RECORDER_FILE_WRAPPER_STAGING_FRAMES];
    seismic_recorder_waveform_file_context_t context = {
        .file = samples_file,
    };

    return seismic_recorder_waveform_read_source(
        seismic_recorder_waveform_file_read_at,
        &context,
        file_size,
        summary,
        start_frame,
        frame_count,
        max_points,
        staging,
        SEISMIC_RECORDER_FILE_WRAPPER_STAGING_FRAMES,
        points,
        point_capacity,
        info
    );
}
