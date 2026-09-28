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

    seismic_recorder_samples_reader_t samples_reader;
    esp_err_t err = seismic_recorder_samples_reader_open(
        read_at,
        read_context,
        file_size,
        summary,
        &samples_reader
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

    err = seismic_recorder_samples_reader_skip(
        read_at,
        read_context,
        &samples_reader,
        start_frame
    );
    if (err != ESP_OK) {
        return err;
    }

    const size_t point_count =
        frame_count < (uint64_t)max_points
            ? (size_t)frame_count
            : max_points;

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
    bool range_has_gaps = false;
    uint64_t range_missing_samples = 0U;
    uint64_t processed = 0U;

    while (processed < frame_count) {
        const uint64_t previous_sample_index = samples_reader.previous_sample_index;
        const bool had_previous_sample = samples_reader.have_previous_sample;
        uint64_t first_sample_index = 0U;
        bool gap_before = false;
        size_t chunk_count = 0U;
        const uint64_t remaining_frames = frame_count - processed;
        const size_t chunk_capacity = remaining_frames < (uint64_t)staging_capacity
            ? (size_t)remaining_frames
            : staging_capacity;
        err = seismic_recorder_samples_reader_next(
            read_at,
            read_context,
            &samples_reader,
            staging,
            chunk_capacity,
            &chunk_count,
            &first_sample_index,
            &gap_before
        );
        if (err != ESP_OK) {
            return err;
        }
        if (chunk_count == 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (gap_before) {
            if (
                !had_previous_sample ||
                previous_sample_index == UINT64_MAX ||
                first_sample_index <= previous_sample_index + 1U
            ) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const uint64_t missing =
                first_sample_index - previous_sample_index - 1U;
            if (missing > UINT64_MAX - range_missing_samples) {
                return ESP_ERR_INVALID_SIZE;
            }
            range_missing_samples += missing;
            range_has_gaps = true;
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

    if (frame_count == source_frame_count - start_frame) {
        err = seismic_recorder_samples_reader_finish(
            read_at,
            read_context,
            &samples_reader
        );
        if (err != ESP_OK) {
            return err;
        }
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
    info->format_version = samples_reader.header.version;
    info->has_gaps =
        summary->has_gaps || summary->dropped_frames > 0U ||
        range_has_gaps || samples_reader.has_gaps;
    info->missing_samples = summary->missing_samples != 0U
        ? summary->missing_samples
        : summary->dropped_frames != 0U
            ? summary->dropped_frames
            : samples_reader.missing_sample_count != 0U
                ? samples_reader.missing_sample_count
                : range_missing_samples;
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


/*
 * Full-resolution counterpart of seismic_recorder_waveform_read_source():
 * copies exact seismic_frame_t records into `frames` instead of reducing
 * them into display envelope points. Offline analysis (MAX-168 source
 * localization) needs every real sample -- arrival-time picking and
 * array-processing backends cannot work from a min/max-reduced waveform.
 * Shares the same header/contract validation and the same bounded, chunked,
 * finite-value/monotonic-timestamp-checked read discipline as the envelope
 * path above; it deliberately does not know about the recorder PSRAM cache
 * or LOD sidecars, both of which are display-scrolling optimizations with no
 * meaning for a one-shot full-session analysis read.
 */
esp_err_t seismic_recorder_waveform_read_raw_source(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t frame_count,
    seismic_frame_t *staging,
    size_t staging_capacity,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count_out
)
{
    if (
        read_at == NULL ||
        summary == NULL ||
        staging == NULL ||
        staging_capacity == 0U ||
        staging_capacity > SIZE_MAX / sizeof(seismic_frame_t) ||
        frames == NULL ||
        frame_capacity == 0U ||
        frame_count_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_count_out = 0U;

    seismic_recorder_samples_reader_t samples_reader;
    esp_err_t err = seismic_recorder_samples_reader_open(
        read_at,
        read_context,
        file_size,
        summary,
        &samples_reader
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
    if (frame_count > (uint64_t)frame_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = seismic_recorder_samples_reader_skip(
        read_at,
        read_context,
        &samples_reader,
        start_frame
    );
    if (err != ESP_OK) {
        return err;
    }

    uint64_t processed = 0U;
    int64_t last_timestamp_us = 0;
    bool have_timestamp = false;

    while (processed < frame_count) {
        const uint64_t remaining = frame_count - processed;
        const size_t chunk_capacity = remaining < (uint64_t)staging_capacity
            ? (size_t)remaining
            : staging_capacity;
        size_t chunk_count = 0U;
        err = seismic_recorder_samples_reader_next(
            read_at,
            read_context,
            &samples_reader,
            staging,
            chunk_capacity,
            &chunk_count,
            NULL,
            NULL
        );
        if (err != ESP_OK) {
            return err;
        }
        if (chunk_count == 0U || (uint64_t)chunk_count > remaining) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        for (size_t chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
            const seismic_frame_t *frame = &staging[chunk_index];

            if (have_timestamp && frame->timestamp_us < last_timestamp_us) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            have_timestamp = true;
            last_timestamp_us = frame->timestamp_us;

            for (size_t channel = 0; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
                if (!isfinite(frame->channel_uv[channel])) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            }

            frames[processed + chunk_index] = *frame;
        }

        processed += (uint64_t)chunk_count;
    }

    if (frame_count == source_frame_count - start_frame) {
        err = seismic_recorder_samples_reader_finish(
            read_at,
            read_context,
            &samples_reader
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    *frame_count_out = (size_t)frame_count;
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
