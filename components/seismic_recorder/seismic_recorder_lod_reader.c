#include "seismic_recorder_lod_reader.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>


static bool seismic_recorder_lod_raw_header_valid(
    const seismic_recorder_binary_header_t *header,
    uint32_t sample_rate_hz
)
{
    return
        header != NULL &&
        memcmp(header->magic, "GEOSEIS1", sizeof(header->magic)) == 0 &&
        header->version == SEISMIC_RECORDER_BINARY_VERSION &&
        header->channel_count == SEISMIC_CHANNEL_COUNT &&
        header->sample_rate_hz == sample_rate_hz &&
        header->sample_rate_hz > 0U &&
        header->frame_size_bytes == sizeof(seismic_frame_t) &&
        header->reserved == 0U;
}


static bool seismic_recorder_lod_mul_u64(
    uint64_t left,
    uint64_t right,
    uint64_t *result
)
{
    if (result == NULL || (left != 0U && right > UINT64_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}


static bool seismic_recorder_lod_add_u64(
    uint64_t left,
    uint64_t right,
    uint64_t *result
)
{
    if (result == NULL || right > UINT64_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}


static bool seismic_recorder_lod_point_for_relative_frame(
    uint64_t relative_frame,
    uint64_t frame_count,
    size_t point_count,
    size_t *point_index
)
{
    if (
        point_index == NULL ||
        frame_count == 0U ||
        point_count == 0U ||
        relative_frame >= frame_count
    ) {
        return false;
    }

    uint64_t numerator = 0U;
    if (!seismic_recorder_lod_mul_u64(
            relative_frame,
            (uint64_t)point_count,
            &numerator
        )) {
        return false;
    }

    const uint64_t mapped = numerator / frame_count;
    if (mapped >= point_count) {
        return false;
    }

    *point_index = (size_t)mapped;
    return true;
}


bool seismic_recorder_lod_block_maps_to_one_point(
    uint64_t request_start_frame,
    uint64_t request_frame_count,
    size_t point_count,
    uint64_t block_first_session_frame,
    size_t *point_index
)
{
    if (
        point_index == NULL ||
        request_frame_count == 0U ||
        point_count == 0U ||
        point_count > request_frame_count ||
        block_first_session_frame % SEISMIC_RECORDER_LOD_BLOCK_FRAMES != 0U
    ) {
        return false;
    }

    uint64_t request_end = 0U;
    uint64_t block_end = 0U;
    if (
        !seismic_recorder_lod_add_u64(
            request_start_frame,
            request_frame_count,
            &request_end
        ) ||
        !seismic_recorder_lod_add_u64(
            block_first_session_frame,
            SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
            &block_end
        ) ||
        block_first_session_frame < request_start_frame ||
        block_end > request_end
    ) {
        return false;
    }

    const uint64_t first_relative =
        block_first_session_frame - request_start_frame;
    const uint64_t last_relative =
        block_end - request_start_frame - 1U;

    size_t first_point = 0U;
    size_t last_point = 0U;
    if (
        !seismic_recorder_lod_point_for_relative_frame(
            first_relative,
            request_frame_count,
            point_count,
            &first_point
        ) ||
        !seismic_recorder_lod_point_for_relative_frame(
            last_relative,
            request_frame_count,
            point_count,
            &last_point
        ) ||
        first_point != last_point
    ) {
        return false;
    }

    *point_index = first_point;
    return true;
}


static void seismic_recorder_lod_points_reset(
    seismic_recorder_waveform_point_t *points,
    size_t point_count
)
{
    for (size_t point = 0U; point < point_count; ++point) {
        points[point].timestamp_us = 0;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            points[point].min_uv[channel] = FLT_MAX;
            points[point].max_uv[channel] = -FLT_MAX;
        }
    }
}


static bool seismic_recorder_lod_record_valid(
    const seismic_recorder_lod_record_t *record,
    uint64_t expected_first_session_frame
)
{
    if (
        record == NULL ||
        record->first_session_frame != expected_first_session_frame ||
        record->frame_count != SEISMIC_RECORDER_LOD_BLOCK_FRAMES ||
        record->reserved != 0U ||
        record->end_timestamp_us < record->start_timestamp_us
    ) {
        return false;
    }

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        if (
            !isfinite(record->min_uv[channel]) ||
            !isfinite(record->max_uv[channel]) ||
            record->min_uv[channel] > record->max_uv[channel]
        ) {
            return false;
        }
    }

    return true;
}


static void seismic_recorder_lod_merge_bounds(
    seismic_recorder_waveform_point_t *point,
    int64_t first_timestamp_us,
    const float min_uv[SEISMIC_CHANNEL_COUNT],
    const float max_uv[SEISMIC_CHANNEL_COUNT]
)
{
    if (point->min_uv[0] == FLT_MAX) {
        point->timestamp_us = first_timestamp_us;
    }

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        if (min_uv[channel] < point->min_uv[channel]) {
            point->min_uv[channel] = min_uv[channel];
        }
        if (max_uv[channel] > point->max_uv[channel]) {
            point->max_uv[channel] = max_uv[channel];
        }
    }
}


static bool seismic_recorder_lod_raw_expected_size(
    uint64_t frame_count,
    uint64_t *size_bytes
)
{
    uint64_t payload_bytes = 0U;
    return
        seismic_recorder_lod_mul_u64(
            frame_count,
            sizeof(seismic_frame_t),
            &payload_bytes
        ) &&
        seismic_recorder_lod_add_u64(
            sizeof(seismic_recorder_binary_header_t),
            payload_bytes,
            size_bytes
        );
}


static esp_err_t seismic_recorder_lod_read_raw_run(
    seismic_recorder_waveform_read_at_fn raw_read_at,
    void *raw_context,
    uint64_t request_start_frame,
    uint64_t request_frame_count,
    size_t point_count,
    uint64_t first_session_frame,
    size_t run_count,
    seismic_frame_t *raw_staging,
    seismic_recorder_waveform_point_t *points,
    bool *have_timestamp,
    int64_t *first_timestamp_us,
    int64_t *last_timestamp_us
)
{
    uint64_t frame_offset_bytes = 0U;
    uint64_t read_offset_bytes = 0U;
    if (
        !seismic_recorder_lod_mul_u64(
            first_session_frame,
            sizeof(seismic_frame_t),
            &frame_offset_bytes
        ) ||
        !seismic_recorder_lod_add_u64(
            sizeof(seismic_recorder_binary_header_t),
            frame_offset_bytes,
            &read_offset_bytes
        ) ||
        run_count > SIZE_MAX / sizeof(seismic_frame_t)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = raw_read_at(
        raw_context,
        read_offset_bytes,
        raw_staging,
        run_count * sizeof(seismic_frame_t)
    );
    if (err != ESP_OK) {
        return err;
    }

    for (size_t index = 0U; index < run_count; ++index) {
        const seismic_frame_t *frame = &raw_staging[index];
        const uint64_t session_frame =
            first_session_frame + (uint64_t)index;
        const uint64_t relative_frame =
            session_frame - request_start_frame;

        size_t point_index = 0U;
        if (!seismic_recorder_lod_point_for_relative_frame(
                relative_frame,
                request_frame_count,
                point_count,
                &point_index
            )) {
            return ESP_ERR_INVALID_SIZE;
        }

        float min_uv[SEISMIC_CHANNEL_COUNT];
        float max_uv[SEISMIC_CHANNEL_COUNT];
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (!isfinite(frame->channel_uv[channel])) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            min_uv[channel] = frame->channel_uv[channel];
            max_uv[channel] = frame->channel_uv[channel];
        }

        if (*have_timestamp && frame->timestamp_us < *last_timestamp_us) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!*have_timestamp) {
            *first_timestamp_us = frame->timestamp_us;
            *have_timestamp = true;
        }
        *last_timestamp_us = frame->timestamp_us;

        seismic_recorder_lod_merge_bounds(
            &points[point_index],
            frame->timestamp_us,
            min_uv,
            max_uv
        );
    }

    return ESP_OK;
}


static esp_err_t seismic_recorder_lod_read_lod_run(
    seismic_recorder_waveform_read_at_fn lod_read_at,
    void *lod_context,
    uint64_t request_start_frame,
    uint64_t request_frame_count,
    size_t point_count,
    uint64_t first_session_frame,
    size_t record_count,
    seismic_recorder_lod_record_t *lod_staging,
    seismic_recorder_waveform_point_t *points,
    bool *have_timestamp,
    int64_t *first_timestamp_us,
    int64_t *last_timestamp_us
)
{
    const uint64_t first_record =
        first_session_frame / SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
    uint64_t record_offset_bytes = 0U;
    uint64_t read_offset_bytes = 0U;
    if (
        !seismic_recorder_lod_mul_u64(
            first_record,
            sizeof(seismic_recorder_lod_record_t),
            &record_offset_bytes
        ) ||
        !seismic_recorder_lod_add_u64(
            sizeof(seismic_recorder_lod_file_header_t),
            record_offset_bytes,
            &read_offset_bytes
        ) ||
        record_count > SIZE_MAX / sizeof(seismic_recorder_lod_record_t)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = lod_read_at(
        lod_context,
        read_offset_bytes,
        lod_staging,
        record_count * sizeof(seismic_recorder_lod_record_t)
    );
    if (err != ESP_OK) {
        return err;
    }

    for (size_t index = 0U; index < record_count; ++index) {
        const uint64_t block_first =
            first_session_frame +
            (uint64_t)index * SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
        seismic_recorder_lod_record_t *record = &lod_staging[index];

        size_t point_index = 0U;
        if (
            !seismic_recorder_lod_record_valid(record, block_first) ||
            !seismic_recorder_lod_block_maps_to_one_point(
                request_start_frame,
                request_frame_count,
                point_count,
                block_first,
                &point_index
            )
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (*have_timestamp && record->start_timestamp_us < *last_timestamp_us) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!*have_timestamp) {
            *first_timestamp_us = record->start_timestamp_us;
            *have_timestamp = true;
        }
        *last_timestamp_us = record->end_timestamp_us;

        seismic_recorder_lod_merge_bounds(
            &points[point_index],
            record->start_timestamp_us,
            record->min_uv,
            record->max_uv
        );
    }

    return ESP_OK;
}


esp_err_t seismic_recorder_lod_read_source(
    seismic_recorder_waveform_read_at_fn raw_read_at,
    void *raw_context,
    uint64_t raw_file_size,
    seismic_recorder_waveform_read_at_fn lod_read_at,
    void *lod_context,
    uint64_t lod_file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_frame_t *raw_staging,
    size_t raw_staging_capacity,
    seismic_recorder_lod_record_t *lod_staging,
    size_t lod_staging_capacity,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
)
{
    if (
        raw_read_at == NULL ||
        lod_read_at == NULL ||
        summary == NULL ||
        !summary->complete ||
        raw_staging == NULL ||
        raw_staging_capacity == 0U ||
        lod_staging == NULL ||
        lod_staging_capacity == 0U ||
        points == NULL ||
        info == NULL ||
        max_points == 0U ||
        max_points > SEISMIC_RECORDER_MAX_WAVEFORM_POINTS ||
        point_capacity < max_points
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    if (start_frame > summary->samples_written) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t available = summary->samples_written - start_frame;
    if (frame_count == 0U) {
        frame_count = available;
    }
    if (frame_count > available) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t expected_raw_size = 0U;
    uint64_t expected_lod_size = 0U;
    if (
        !seismic_recorder_lod_raw_expected_size(
            summary->samples_written,
            &expected_raw_size
        ) ||
        !seismic_recorder_lod_expected_file_size(
            summary->samples_written,
            &expected_lod_size
        ) ||
        raw_file_size != expected_raw_size ||
        lod_file_size != expected_lod_size
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    seismic_recorder_binary_header_t raw_header;
    esp_err_t err = raw_read_at(
        raw_context,
        0U,
        &raw_header,
        sizeof(raw_header)
    );
    if (
        err != ESP_OK ||
        !seismic_recorder_lod_raw_header_valid(
            &raw_header,
            summary->sample_rate_hz
        )
    ) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    seismic_recorder_lod_file_header_t lod_header;
    err = lod_read_at(
        lod_context,
        0U,
        &lod_header,
        sizeof(lod_header)
    );
    if (
        err != ESP_OK ||
        !seismic_recorder_lod_header_valid(
            &lod_header,
            summary->sample_rate_hz
        ) ||
        !seismic_recorder_lod_header_is_complete(&lod_header)
    ) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    const size_t point_count =
        frame_count < (uint64_t)max_points
            ? (size_t)frame_count
            : max_points;
    seismic_recorder_lod_points_reset(points, point_count);

    bool have_timestamp = false;
    int64_t first_timestamp_us = 0;
    int64_t last_timestamp_us = 0;
    uint64_t cursor = start_frame;
    const uint64_t request_end = start_frame + frame_count;

    while (cursor < request_end) {
        size_t point_index = 0U;
        const bool can_use_lod =
            cursor % SEISMIC_RECORDER_LOD_BLOCK_FRAMES == 0U &&
            seismic_recorder_lod_block_maps_to_one_point(
                start_frame,
                frame_count,
                point_count,
                cursor,
                &point_index
            );
        (void)point_index;

        if (can_use_lod) {
            size_t record_count = 0U;
            uint64_t probe = cursor;
            while (record_count < lod_staging_capacity) {
                size_t ignored_point = 0U;
                if (!seismic_recorder_lod_block_maps_to_one_point(
                        start_frame,
                        frame_count,
                        point_count,
                        probe,
                        &ignored_point
                    )) {
                    break;
                }

                ++record_count;
                if (
                    probe >
                    UINT64_MAX - SEISMIC_RECORDER_LOD_BLOCK_FRAMES
                ) {
                    break;
                }
                probe += SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
            }

            if (record_count == 0U) {
                return ESP_ERR_INVALID_STATE;
            }

            err = seismic_recorder_lod_read_lod_run(
                lod_read_at,
                lod_context,
                start_frame,
                frame_count,
                point_count,
                cursor,
                record_count,
                lod_staging,
                points,
                &have_timestamp,
                &first_timestamp_us,
                &last_timestamp_us
            );
            if (err != ESP_OK) {
                return err;
            }

            cursor +=
                (uint64_t)record_count *
                SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
            continue;
        }

        size_t raw_count = 0U;
        while (
            cursor + raw_count < request_end &&
            raw_count < raw_staging_capacity
        ) {
            const uint64_t probe = cursor + (uint64_t)raw_count;
            size_t ignored_point = 0U;
            if (
                raw_count > 0U &&
                probe % SEISMIC_RECORDER_LOD_BLOCK_FRAMES == 0U &&
                seismic_recorder_lod_block_maps_to_one_point(
                    start_frame,
                    frame_count,
                    point_count,
                    probe,
                    &ignored_point
                )
            ) {
                break;
            }

            size_t step = 1U;
            const uint64_t alignment =
                probe % SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
            if (alignment == 0U) {
                step = SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
            }
            else {
                step = (size_t)(
                    SEISMIC_RECORDER_LOD_BLOCK_FRAMES - alignment
                );
            }

            const uint64_t remaining = request_end - probe;
            if ((uint64_t)step > remaining) {
                step = (size_t)remaining;
            }
            if (step > raw_staging_capacity - raw_count) {
                step = raw_staging_capacity - raw_count;
            }
            raw_count += step;
        }

        if (raw_count == 0U) {
            return ESP_ERR_INVALID_STATE;
        }

        err = seismic_recorder_lod_read_raw_run(
            raw_read_at,
            raw_context,
            start_frame,
            frame_count,
            point_count,
            cursor,
            raw_count,
            raw_staging,
            points,
            &have_timestamp,
            &first_timestamp_us,
            &last_timestamp_us
        );
        if (err != ESP_OK) {
            return err;
        }
        cursor += raw_count;
    }

    info->complete = summary->complete;
    snprintf(info->object_id, sizeof(info->object_id), "%s", "");
    snprintf(info->session_id, sizeof(info->session_id), "%s", summary->session_id);
    snprintf(info->source_name, sizeof(info->source_name), "%s", summary->source_name);
    info->sample_rate_hz = raw_header.sample_rate_hz;
    info->channel_count = (uint8_t)raw_header.channel_count;
    info->samples_written = summary->samples_written;
    info->dropped_frames = summary->dropped_frames;
    info->recording_ms = summary->recording_ms;
    info->source_frame_count = summary->samples_written;
    info->start_frame = start_frame;
    info->frame_count = frame_count;
    info->point_count = point_count;

    if (have_timestamp) {
        info->start_timestamp_us = first_timestamp_us;
        info->end_timestamp_us = last_timestamp_us;
    }

    return ESP_OK;
}
