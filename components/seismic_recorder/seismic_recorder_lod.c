#include "seismic_recorder_lod.h"

#include <math.h>
#include <string.h>


_Static_assert(
    sizeof(seismic_recorder_lod_file_header_t) ==
        SEISMIC_RECORDER_LOD_FILE_HEADER_BYTES,
    "LOD file header layout changed"
);

_Static_assert(
    sizeof(seismic_recorder_lod_record_t) ==
        SEISMIC_RECORDER_LOD_RECORD_BYTES,
    "LOD record layout changed"
);


static const char s_lod_magic[8] = {
    'G', 'E', 'O', 'L', 'O', 'D', '6', '4'
};


static bool seismic_recorder_lod_frames_valid(
    const seismic_frame_t *frames,
    size_t frame_count,
    const seismic_recorder_lod_builder_t *builder
)
{
    int64_t previous_timestamp =
        builder->pending_count > 0U
            ? builder->pending_end_timestamp_us
            : 0;
    bool have_previous = builder->pending_count > 0U;

    for (size_t index = 0U; index < frame_count; ++index) {
        const seismic_frame_t *frame = &frames[index];

        if (have_previous && frame->timestamp_us < previous_timestamp) {
            return false;
        }

        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            if (!isfinite(frame->channel_uv[channel])) {
                return false;
            }
        }

        previous_timestamp = frame->timestamp_us;
        have_previous = true;
    }

    return true;
}


void seismic_recorder_lod_header_init(
    seismic_recorder_lod_file_header_t *header,
    uint32_t sample_rate_hz
)
{
    if (header == NULL) {
        return;
    }

    memset(header, 0, sizeof(*header));
    memcpy(header->magic, s_lod_magic, sizeof(header->magic));
    header->version = SEISMIC_RECORDER_LOD_FILE_VERSION;
    header->channel_count = SEISMIC_CHANNEL_COUNT;
    header->block_frames = SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
    header->sample_rate_hz = sample_rate_hz;
    header->frame_size_bytes = sizeof(seismic_frame_t);
    header->record_size_bytes = sizeof(seismic_recorder_lod_record_t);
    header->flags = 0U;
}


bool seismic_recorder_lod_header_valid(
    const seismic_recorder_lod_file_header_t *header,
    uint32_t expected_sample_rate_hz
)
{
    return
        header != NULL &&
        memcmp(header->magic, s_lod_magic, sizeof(header->magic)) == 0 &&
        header->version == SEISMIC_RECORDER_LOD_FILE_VERSION &&
        header->channel_count == SEISMIC_CHANNEL_COUNT &&
        header->block_frames == SEISMIC_RECORDER_LOD_BLOCK_FRAMES &&
        header->sample_rate_hz == expected_sample_rate_hz &&
        header->sample_rate_hz > 0U &&
        header->frame_size_bytes == sizeof(seismic_frame_t) &&
        header->record_size_bytes == sizeof(seismic_recorder_lod_record_t) &&
        (header->flags & ~SEISMIC_RECORDER_LOD_FLAG_COMPLETE) == 0U;
}


void seismic_recorder_lod_header_mark_complete(
    seismic_recorder_lod_file_header_t *header
)
{
    if (header != NULL) {
        header->flags |= SEISMIC_RECORDER_LOD_FLAG_COMPLETE;
    }
}


bool seismic_recorder_lod_header_is_complete(
    const seismic_recorder_lod_file_header_t *header
)
{
    return
        header != NULL &&
        (header->flags & SEISMIC_RECORDER_LOD_FLAG_COMPLETE) != 0U;
}


bool seismic_recorder_lod_expected_file_size(
    uint64_t persisted_session_frames,
    uint64_t *size_bytes
)
{
    if (size_bytes == NULL) {
        return false;
    }

    const uint64_t record_count =
        persisted_session_frames / SEISMIC_RECORDER_LOD_BLOCK_FRAMES;

    if (
        record_count >
            (UINT64_MAX - SEISMIC_RECORDER_LOD_FILE_HEADER_BYTES) /
                SEISMIC_RECORDER_LOD_RECORD_BYTES
    ) {
        return false;
    }

    *size_bytes =
        SEISMIC_RECORDER_LOD_FILE_HEADER_BYTES +
        record_count * SEISMIC_RECORDER_LOD_RECORD_BYTES;
    return true;
}


void seismic_recorder_lod_builder_reset(
    seismic_recorder_lod_builder_t *builder,
    uint64_t next_session_frame
)
{
    if (builder == NULL) {
        return;
    }

    memset(builder, 0, sizeof(*builder));
    builder->next_session_frame = next_session_frame;
}


static void seismic_recorder_lod_pending_start(
    seismic_recorder_lod_builder_t *builder,
    uint64_t session_frame,
    const seismic_frame_t *frame
)
{
    builder->pending_first_session_frame = session_frame;
    builder->pending_start_timestamp_us = frame->timestamp_us;
    builder->pending_end_timestamp_us = frame->timestamp_us;

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        builder->pending_min_uv[channel] = frame->channel_uv[channel];
        builder->pending_max_uv[channel] = frame->channel_uv[channel];
    }
}


static void seismic_recorder_lod_pending_add(
    seismic_recorder_lod_builder_t *builder,
    const seismic_frame_t *frame
)
{
    builder->pending_end_timestamp_us = frame->timestamp_us;

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        if (frame->channel_uv[channel] < builder->pending_min_uv[channel]) {
            builder->pending_min_uv[channel] = frame->channel_uv[channel];
        }
        if (frame->channel_uv[channel] > builder->pending_max_uv[channel]) {
            builder->pending_max_uv[channel] = frame->channel_uv[channel];
        }
    }
}


static void seismic_recorder_lod_emit_pending(
    const seismic_recorder_lod_builder_t *builder,
    seismic_recorder_lod_record_t *record
)
{
    memset(record, 0, sizeof(*record));
    record->first_session_frame = builder->pending_first_session_frame;
    record->start_timestamp_us = builder->pending_start_timestamp_us;
    record->end_timestamp_us = builder->pending_end_timestamp_us;
    record->frame_count = SEISMIC_RECORDER_LOD_BLOCK_FRAMES;

    memcpy(
        record->min_uv,
        builder->pending_min_uv,
        sizeof(record->min_uv)
    );
    memcpy(
        record->max_uv,
        builder->pending_max_uv,
        sizeof(record->max_uv)
    );
}


esp_err_t seismic_recorder_lod_builder_append(
    seismic_recorder_lod_builder_t *builder,
    uint64_t first_session_frame,
    const seismic_frame_t *frames,
    size_t frame_count,
    seismic_recorder_lod_record_t *records,
    size_t record_capacity,
    size_t *record_count
)
{
    if (
        builder == NULL ||
        frames == NULL ||
        frame_count == 0U ||
        records == NULL ||
        record_count == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *record_count = 0U;

    if (first_session_frame != builder->next_session_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((uint64_t)frame_count > UINT64_MAX - first_session_frame) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (frame_count > SIZE_MAX - builder->pending_count) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t emitted =
        (builder->pending_count + frame_count) /
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
    if (emitted > record_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!seismic_recorder_lod_frames_valid(frames, frame_count, builder)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t output_index = 0U;
    for (size_t index = 0U; index < frame_count; ++index) {
        const uint64_t session_frame =
            first_session_frame + (uint64_t)index;
        const seismic_frame_t *frame = &frames[index];

        if (builder->pending_count == 0U) {
            seismic_recorder_lod_pending_start(
                builder,
                session_frame,
                frame
            );
        }
        else {
            seismic_recorder_lod_pending_add(builder, frame);
        }

        ++builder->pending_count;
        ++builder->next_session_frame;

        if (builder->pending_count == SEISMIC_RECORDER_LOD_BLOCK_FRAMES) {
            seismic_recorder_lod_emit_pending(
                builder,
                &records[output_index]
            );
            ++output_index;
            builder->pending_count = 0U;
        }
    }

    *record_count = output_index;
    return ESP_OK;
}
