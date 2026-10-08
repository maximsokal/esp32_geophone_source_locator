#include "seismic_recorder_internal.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>


static bool seismic_recorder_samples_add_u64(
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


static bool seismic_recorder_samples_mul_u64(
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


static bool seismic_recorder_samples_metadata_valid(
    const seismic_recorder_binary_v2_metadata_t *metadata
)
{
    if (
        metadata == NULL ||
        !isfinite(metadata->input_range_v) ||
        metadata->input_range_v <= 0.0f
    ) {
        return false;
    }

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        if (
            !isfinite(metadata->scale_uv_per_lsb[channel]) ||
            metadata->scale_uv_per_lsb[channel] <= 0.0f ||
            !isfinite(metadata->offset_uv[channel])
        ) {
            return false;
        }
    }
    return true;
}


static bool seismic_recorder_samples_format_valid(
    const seismic_recorder_sample_format_t *format
)
{
    if (
        format == NULL ||
        format->version != SEISMIC_RECORDER_BINARY_VERSION_V2 ||
        format->sample_rate_hz == 0U
    ) {
        return false;
    }

    seismic_recorder_binary_v2_metadata_t metadata;
    seismic_recorder_samples_v2_metadata_from_format(format, &metadata);
    return seismic_recorder_samples_metadata_valid(&metadata);
}


esp_err_t seismic_recorder_samples_format_from_acquisition(
    const seismic_acquisition_snapshot_t *acquisition,
    seismic_recorder_sample_format_t *format
)
{
    if (acquisition == NULL || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(format, 0, sizeof(*format));
    format->sample_rate_hz = acquisition->sample_rate_hz;
    format->version =
        acquisition->source_name != NULL &&
        strcmp(acquisition->source_name, "AD7606") == 0
            ? SEISMIC_RECORDER_BINARY_VERSION_V2
            : SEISMIC_RECORDER_BINARY_VERSION_V1;

    if (format->version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        return ESP_OK;
    }
    if (!isfinite(acquisition->input_range_v) || acquisition->input_range_v <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    /* AD7606 reports signed 16-bit codes over the configured +/- range. */
    const float scale_uv_per_lsb =
        acquisition->input_range_v * 1000000.0f / 32768.0f;
    if (!isfinite(scale_uv_per_lsb) || scale_uv_per_lsb <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        format->scale_uv_per_lsb[channel] = scale_uv_per_lsb;
        format->offset_uv[channel] = 0.0f;
    }
    format->input_range_v = acquisition->input_range_v;
    format->oversampling_ratio = acquisition->oversampling_ratio;
    format->spi_clock_hz = acquisition->bus_clock_hz;
    return ESP_OK;
}


void seismic_recorder_samples_v2_metadata_from_format(
    const seismic_recorder_sample_format_t *format,
    seismic_recorder_binary_v2_metadata_t *metadata
)
{
    if (format == NULL || metadata == NULL) {
        return;
    }

    memset(metadata, 0, sizeof(*metadata));
    memcpy(metadata->scale_uv_per_lsb, format->scale_uv_per_lsb, sizeof(metadata->scale_uv_per_lsb));
    memcpy(metadata->offset_uv, format->offset_uv, sizeof(metadata->offset_uv));
    metadata->input_range_v = format->input_range_v;
    metadata->oversampling_ratio = format->oversampling_ratio;
    metadata->spi_clock_hz = format->spi_clock_hz;
}


static void seismic_recorder_samples_put_le16(
    uint8_t *destination,
    int16_t value
)
{
    const uint16_t raw = (uint16_t)value;
    destination[0] = (uint8_t)(raw & 0xffU);
    destination[1] = (uint8_t)(raw >> 8U);
}


static int16_t seismic_recorder_samples_get_le16(const uint8_t *source)
{
    const uint16_t raw = (uint16_t)source[0] | (uint16_t)((uint16_t)source[1] << 8U);
    int16_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}


#if defined(SEISMIC_RECORDER_SAMPLES_HOST_TEST)
static size_t s_test_quantizer_call_count;
#endif


static bool seismic_recorder_samples_quantize(
    float value_uv,
    float scale_uv_per_lsb,
    float offset_uv,
    int16_t *raw
)
{
#if defined(SEISMIC_RECORDER_SAMPLES_HOST_TEST)
    ++s_test_quantizer_call_count;
#endif
    if (
        raw == NULL ||
        !isfinite(value_uv) ||
        !isfinite(scale_uv_per_lsb) ||
        scale_uv_per_lsb <= 0.0f ||
        !isfinite(offset_uv)
    ) {
        return false;
    }

    const double rounded = round(
        ((double)value_uv - (double)offset_uv) /
        (double)scale_uv_per_lsb
    );
    if (rounded <= (double)INT16_MIN) {
        *raw = INT16_MIN;
    }
    else if (rounded >= (double)INT16_MAX) {
        *raw = INT16_MAX;
    }
    else {
        *raw = (int16_t)rounded;
    }
    return true;
}


esp_err_t seismic_recorder_samples_quantize_frame(
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frame,
    seismic_frame_t *quantized_frame
)
{
    if (format == NULL || frame == NULL || quantized_frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *quantized_frame = *frame;
    if (format->version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        return ESP_OK;
    }
    if (!seismic_recorder_samples_format_valid(format)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        int16_t raw = 0;
        if (!seismic_recorder_samples_quantize(
                frame->channel_uv[channel],
                format->scale_uv_per_lsb[channel],
                format->offset_uv[channel],
                &raw
            )) {
            return ESP_ERR_INVALID_ARG;
        }
        quantized_frame->channel_uv[channel] =
            (float)raw * format->scale_uv_per_lsb[channel] +
            format->offset_uv[channel];
    }
    return ESP_OK;
}


bool seismic_recorder_samples_timestamp_at(
    uint64_t first_timestamp_us,
    uint64_t sample_offset,
    uint32_t sample_rate_hz,
    uint64_t *timestamp_us
)
{
    if (timestamp_us == NULL || sample_rate_hz == 0U) {
        return false;
    }

    uint64_t scaled_offset = 0U;
    uint64_t result = 0U;
    if (
        !seismic_recorder_samples_mul_u64(
            sample_offset,
            1000000U,
            &scaled_offset
        ) ||
        !seismic_recorder_samples_add_u64(
            first_timestamp_us,
            scaled_offset / sample_rate_hz,
            &result
        )
    ) {
        return false;
    }

    *timestamp_us = result;
    return true;
}


esp_err_t seismic_recorder_samples_encode_v2_block(
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    bool gap_before,
    void *buffer,
    size_t buffer_capacity,
    size_t *bytes_written
)
{
    if (
        !seismic_recorder_samples_format_valid(format) ||
        frames == NULL ||
        sample_indices == NULL ||
        frame_count == 0U ||
        frame_count > SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES ||
        buffer == NULL ||
        bytes_written == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *bytes_written = 0U;
    const size_t payload_size = frame_count * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
    if (
        payload_size > SIZE_MAX - SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES ||
        buffer_capacity < SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + payload_size ||
        frames[0].timestamp_us < 0
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t first_sample_index = sample_indices[0];
    if (first_sample_index > UINT64_MAX - ((uint64_t)frame_count - 1U)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t last_timestamp_us = 0U;
    if (
        !seismic_recorder_samples_timestamp_at(
            (uint64_t)frames[0].timestamp_us,
            (uint64_t)frame_count - 1U,
            format->sample_rate_hz,
            &last_timestamp_us
        ) ||
        last_timestamp_us > (uint64_t)INT64_MAX
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < frame_count; ++index) {
        if (
            sample_indices[index] != first_sample_index + (uint64_t)index ||
            (index > 0U &&
             frames[index].timestamp_us < frames[index - 1U].timestamp_us) ||
            !isfinite(frames[index].channel_uv[0]) ||
            !isfinite(frames[index].channel_uv[1]) ||
            !isfinite(frames[index].channel_uv[2]) ||
            !isfinite(frames[index].channel_uv[3])
        ) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    seismic_recorder_binary_v2_block_header_t header = {
        .first_sample_index = first_sample_index,
        .first_timestamp_us = (uint64_t)frames[0].timestamp_us,
        .frame_count = (uint16_t)frame_count,
        .flags = gap_before ? SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE : 0U,
    };
    uint8_t *bytes = (uint8_t *)buffer;
    memcpy(bytes, &header, sizeof(header));

    uint8_t *payload = bytes + sizeof(header);
    for (size_t frame = 0U; frame < frame_count; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            int16_t raw = 0;
            if (!seismic_recorder_samples_quantize(
                    frames[frame].channel_uv[channel],
                    format->scale_uv_per_lsb[channel],
                    format->offset_uv[channel],
                    &raw
                )) {
                return ESP_ERR_INVALID_ARG;
            }
            seismic_recorder_samples_put_le16(
                payload + frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * sizeof(raw),
                raw
            );
        }
    }

    *bytes_written = sizeof(header) + payload_size;
    return ESP_OK;
}


static bool seismic_recorder_samples_header_valid(
    const seismic_recorder_binary_header_t *header,
    const seismic_recorder_binary_v2_metadata_t *metadata,
    uint32_t expected_sample_rate_hz,
    uint16_t expected_channel_count
)
{
    if (
        header == NULL ||
        memcmp(header->magic, "GEOSEIS1", sizeof(header->magic)) != 0 ||
        header->sample_rate_hz == 0U ||
        header->sample_rate_hz != expected_sample_rate_hz ||
        header->channel_count != expected_channel_count ||
        header->channel_count != SEISMIC_CHANNEL_COUNT ||
        header->reserved != 0U
    ) {
        return false;
    }

    if (header->version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        return header->frame_size_bytes == sizeof(seismic_frame_t);
    }
    return
        header->version == SEISMIC_RECORDER_BINARY_VERSION_V2 &&
        header->frame_size_bytes == SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES &&
        seismic_recorder_samples_metadata_valid(metadata);
}


esp_err_t seismic_recorder_samples_reader_open(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    uint64_t file_size,
    const seismic_recorder_session_summary_t *summary,
    seismic_recorder_samples_reader_t *reader
)
{
    if (read_at == NULL || summary == NULL || reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(reader, 0, sizeof(*reader));

    if (file_size < sizeof(reader->header) || file_size > (uint64_t)LONG_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t err = read_at(
        read_context,
        0U,
        &reader->header,
        sizeof(reader->header)
    );
    if (err != ESP_OK) {
        return err;
    }

    if (
        reader->header.version != SEISMIC_RECORDER_BINARY_VERSION_V1 &&
        reader->header.version != SEISMIC_RECORDER_BINARY_VERSION_V2
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
        if (file_size < sizeof(reader->header) + sizeof(reader->v2_metadata)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        err = read_at(
            read_context,
            sizeof(reader->header),
            &reader->v2_metadata,
            sizeof(reader->v2_metadata)
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    if (
        summary->sample_rate_hz == 0U ||
        summary->channel_count != SEISMIC_CHANNEL_COUNT ||
        (summary->format_version != 0U &&
         summary->format_version != reader->header.version) ||
        !seismic_recorder_samples_header_valid(
            &reader->header,
            &reader->v2_metadata,
            summary->sample_rate_hz,
            summary->channel_count
        )
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    reader->file_size_bytes = file_size;
    reader->logical_frame_count = summary->samples_written;
    reader->require_exact_file_size = summary->complete || summary->recovered;
    reader->data_offset_bytes = sizeof(reader->header) +
        (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V2
            ? sizeof(reader->v2_metadata)
            : 0U);
    reader->next_block_offset_bytes = reader->data_offset_bytes;

    if (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        uint64_t payload_bytes = 0U;
        uint64_t expected_size = 0U;
        if (
            !seismic_recorder_samples_mul_u64(
                summary->samples_written,
                sizeof(seismic_frame_t),
                &payload_bytes
            ) ||
            !seismic_recorder_samples_add_u64(
                reader->data_offset_bytes,
                payload_bytes,
                &expected_size
            ) ||
            file_size < expected_size ||
            ((summary->complete || summary->recovered) &&
             file_size != expected_size)
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}


static esp_err_t seismic_recorder_samples_reader_load_block(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader
)
{
    if (reader->block_loaded) {
        return ESP_OK;
    }
    if (
        reader->logical_frame_position >= reader->logical_frame_count ||
        reader->next_block_offset_bytes >= reader->file_size_bytes
    ) {
        return ESP_ERR_NOT_FOUND;
    }

    uint64_t header_end = 0U;
    if (
        !seismic_recorder_samples_add_u64(
            reader->next_block_offset_bytes,
            sizeof(seismic_recorder_binary_v2_block_header_t),
            &header_end
        ) ||
        header_end > reader->file_size_bytes
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    seismic_recorder_binary_v2_block_header_t block;
    esp_err_t err = read_at(
        read_context,
        reader->next_block_offset_bytes,
        &block,
        sizeof(block)
    );
    if (err != ESP_OK) {
        return err;
    }

    uint64_t payload_bytes = 0U;
    uint64_t block_end = 0U;
    if (
        block.frame_count == 0U ||
        block.frame_count > SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES ||
        (block.flags & ~SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U ||
        (uint64_t)block.frame_count >
            reader->logical_frame_count - reader->logical_frame_position ||
        !seismic_recorder_samples_mul_u64(
            block.frame_count,
            SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
            &payload_bytes
        ) ||
        !seismic_recorder_samples_add_u64(header_end, payload_bytes, &block_end) ||
        block_end > reader->file_size_bytes
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (reader->have_previous_sample) {
        if (reader->previous_sample_index == UINT64_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint64_t expected_index = reader->previous_sample_index + 1U;
        const bool gap_before =
            (block.flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U;
        if (
            (gap_before && block.first_sample_index <= expected_index) ||
            (!gap_before && block.first_sample_index != expected_index) ||
            block.first_timestamp_us < reader->previous_timestamp_us
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    else if ((block.flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t block_last_timestamp = 0U;
    if (
        block.first_timestamp_us > (uint64_t)INT64_MAX ||
        !seismic_recorder_samples_timestamp_at(
            block.first_timestamp_us,
            (uint64_t)block.frame_count - 1U,
            reader->header.sample_rate_hz,
            &block_last_timestamp
        ) ||
        block_last_timestamp > (uint64_t)INT64_MAX ||
        block.first_sample_index >
            UINT64_MAX - ((uint64_t)block.frame_count - 1U)
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (reader->have_previous_sample &&
        (block.flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U) {
        const uint64_t missing =
            block.first_sample_index - reader->previous_sample_index - 1U;
        if (missing > UINT64_MAX - reader->missing_sample_count) {
            return ESP_ERR_INVALID_SIZE;
        }
        reader->missing_sample_count += missing;
        reader->has_gaps = true;
    }

    reader->block_first_sample_index = block.first_sample_index;
    reader->block_first_timestamp_us = block.first_timestamp_us;
    reader->block_frame_count = block.frame_count;
    reader->block_consumed = 0U;
    reader->block_flags = block.flags;
    reader->block_data_offset_bytes = header_end;
    reader->block_loaded = true;
    return ESP_OK;
}


static esp_err_t seismic_recorder_samples_reader_advance_block(
    seismic_recorder_samples_reader_t *reader
)
{
    uint64_t payload_bytes = 0U;
    uint64_t next_offset = 0U;
    uint64_t last_timestamp = 0U;
    if (
        !seismic_recorder_samples_mul_u64(
            reader->block_frame_count,
            SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
            &payload_bytes
        ) ||
        !seismic_recorder_samples_add_u64(
            reader->block_data_offset_bytes,
            payload_bytes,
            &next_offset
        ) ||
        !seismic_recorder_samples_timestamp_at(
            reader->block_first_timestamp_us,
            (uint64_t)reader->block_frame_count - 1U,
            reader->header.sample_rate_hz,
            &last_timestamp
        ) ||
        reader->block_first_sample_index >
            UINT64_MAX - ((uint64_t)reader->block_frame_count - 1U)
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    reader->previous_sample_index =
        reader->block_first_sample_index + (uint64_t)reader->block_frame_count - 1U;
    reader->previous_timestamp_us = last_timestamp;
    reader->have_previous_sample = true;
    reader->next_block_offset_bytes = next_offset;
    reader->block_loaded = false;
    return ESP_OK;
}


esp_err_t seismic_recorder_samples_reader_skip(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    uint64_t frame_count
)
{
    if (read_at == NULL || reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (
        reader->logical_frame_position > reader->logical_frame_count ||
        frame_count > reader->logical_frame_count - reader->logical_frame_position
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    /* The caller passes an absolute frame count to skip from the current cursor. */
    uint64_t remaining = frame_count;
    while (remaining > 0U) {
        if (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
            uint64_t byte_offset = 0U;
            uint64_t next_position = 0U;
            if (
                !seismic_recorder_samples_mul_u64(
                    reader->logical_frame_position + remaining,
                    sizeof(seismic_frame_t),
                    &byte_offset
                ) ||
                !seismic_recorder_samples_add_u64(
                    reader->data_offset_bytes,
                    byte_offset,
                    &next_position
                ) ||
                next_position > reader->file_size_bytes
            ) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            reader->logical_frame_position += remaining;
            return ESP_OK;
        }

        esp_err_t err = seismic_recorder_samples_reader_load_block(
            read_at,
            read_context,
            reader
        );
        if (err != ESP_OK) {
            return err;
        }
        const size_t available =
            (size_t)(reader->block_frame_count - reader->block_consumed);
        const size_t skipped = remaining < (uint64_t)available
            ? (size_t)remaining
            : available;

        if (reader->block_first_sample_index >
                UINT64_MAX - ((uint64_t)reader->block_consumed + skipped - 1U)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        reader->previous_sample_index = reader->block_first_sample_index +
            (uint64_t)reader->block_consumed + (uint64_t)skipped - 1U;
        if (!seismic_recorder_samples_timestamp_at(
                reader->block_first_timestamp_us,
                (uint64_t)reader->block_consumed + (uint64_t)skipped - 1U,
                reader->header.sample_rate_hz,
                &reader->previous_timestamp_us
            ) || reader->previous_timestamp_us > (uint64_t)INT64_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        reader->have_previous_sample = true;
        reader->block_consumed = (uint16_t)(reader->block_consumed + skipped);
        reader->logical_frame_position += (uint64_t)skipped;
        remaining -= (uint64_t)skipped;

        if (reader->block_consumed == reader->block_frame_count) {
            err = seismic_recorder_samples_reader_advance_block(reader);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_samples_reader_next(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count,
    uint64_t *first_sample_index,
    bool *gap_before
)
{
    return seismic_recorder_samples_reader_next_ex(
        read_at, read_context, reader, frames, frame_capacity, frame_count,
        first_sample_index, gap_before, NULL, NULL
    );
}


esp_err_t seismic_recorder_samples_reader_next_ex(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader,
    seismic_frame_t *frames,
    size_t frame_capacity,
    size_t *frame_count,
    uint64_t *first_sample_index,
    bool *gap_before,
    size_t rail_sample_counts[SEISMIC_CHANNEL_COUNT],
    size_t *samples_examined
)
{
    if (
        read_at == NULL || reader == NULL || frames == NULL ||
        frame_capacity == 0U || frame_count == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame_count = 0U;
    if (rail_sample_counts != NULL) {
        memset(rail_sample_counts, 0, sizeof(size_t) * SEISMIC_CHANNEL_COUNT);
    }
    if (samples_examined != NULL) {
        *samples_examined = 0U;
    }
    esp_err_t err = ESP_OK;
    if (first_sample_index != NULL) {
        *first_sample_index = 0U;
    }
    if (gap_before != NULL) {
        *gap_before = false;
    }

    if (reader->logical_frame_position >= reader->logical_frame_count) {
        return ESP_OK;
    }

    if (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        const size_t count =
            reader->logical_frame_count - reader->logical_frame_position <
                (uint64_t)frame_capacity
                ? (size_t)(reader->logical_frame_count - reader->logical_frame_position)
                : frame_capacity;
        uint64_t frame_offset = 0U;
        uint64_t byte_offset = 0U;
        if (
            !seismic_recorder_samples_mul_u64(
                reader->logical_frame_position,
                sizeof(seismic_frame_t),
                &frame_offset
            ) ||
            !seismic_recorder_samples_add_u64(
                reader->data_offset_bytes,
                frame_offset,
                &byte_offset
            )
        ) {
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t bytes = count * sizeof(seismic_frame_t);
        err = read_at(read_context, byte_offset, frames, bytes);
        if (err != ESP_OK) {
            return err;
        }
        if (first_sample_index != NULL) {
            *first_sample_index = reader->logical_frame_position;
        }
        reader->logical_frame_position += (uint64_t)count;
        *frame_count = count;
        return ESP_OK;
    }

    err = seismic_recorder_samples_reader_load_block(
        read_at,
        read_context,
        reader
    );
    if (err == ESP_ERR_NOT_FOUND) {
        return reader->logical_frame_count == 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    if (err != ESP_OK) {
        return err;
    }

    const size_t remaining =
        (size_t)(reader->block_frame_count - reader->block_consumed);
    const size_t count = remaining < frame_capacity ? remaining : frame_capacity;
    if (count == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t source_offset =
        reader->block_data_offset_bytes +
        (uint64_t)reader->block_consumed * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
    const size_t payload_size = count * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
    err = read_at(read_context, source_offset, frames, payload_size);
    if (err != ESP_OK) {
        return err;
    }

    /* Decode backwards because each float frame is wider than its int16 input. */
    uint8_t *packed = (uint8_t *)frames;
    for (size_t reverse = count; reverse > 0U; --reverse) {
        const size_t index = reverse - 1U;
        seismic_frame_t decoded = {0};
        const uint64_t sample_offset =
            (uint64_t)reader->block_consumed + (uint64_t)index;
        uint64_t timestamp_us = 0U;
        if (!seismic_recorder_samples_timestamp_at(
                reader->block_first_timestamp_us,
                sample_offset,
                reader->header.sample_rate_hz,
                &timestamp_us
            ) || timestamp_us > (uint64_t)INT64_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        decoded.timestamp_us = (int64_t)timestamp_us;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const int16_t raw = seismic_recorder_samples_get_le16(
                packed + index * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * sizeof(raw)
            );
            if (
                rail_sample_counts != NULL &&
                (raw == INT16_MIN || raw == INT16_MAX)
            ) {
                ++rail_sample_counts[channel];
            }
            decoded.channel_uv[channel] =
                (float)raw * reader->v2_metadata.scale_uv_per_lsb[channel] +
                reader->v2_metadata.offset_uv[channel];
        }
        if (samples_examined != NULL) {
            ++*samples_examined;
        }
        frames[index] = decoded;
    }

    if (first_sample_index != NULL) {
        *first_sample_index =
            reader->block_first_sample_index + reader->block_consumed;
    }
    if (gap_before != NULL && reader->block_consumed == 0U) {
        *gap_before =
            (reader->block_flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U;
    }

    const uint64_t last_offset =
        (uint64_t)reader->block_consumed + (uint64_t)count - 1U;
    if (
        reader->block_first_sample_index > UINT64_MAX - last_offset ||
        !seismic_recorder_samples_timestamp_at(
            reader->block_first_timestamp_us,
            last_offset,
            reader->header.sample_rate_hz,
            &reader->previous_timestamp_us
        ) ||
        reader->previous_timestamp_us > (uint64_t)INT64_MAX
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    reader->previous_sample_index = reader->block_first_sample_index + last_offset;
    reader->have_previous_sample = true;
    reader->block_consumed = (uint16_t)(reader->block_consumed + count);
    reader->logical_frame_position += (uint64_t)count;
    *frame_count = count;

    if (reader->block_consumed == reader->block_frame_count) {
        err = seismic_recorder_samples_reader_advance_block(reader);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_samples_reader_finish(
    seismic_recorder_waveform_read_at_fn read_at,
    void *read_context,
    seismic_recorder_samples_reader_t *reader
)
{
    if (read_at == NULL || reader == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (reader->header.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        return ESP_OK;
    }

    if (reader->logical_frame_position < reader->logical_frame_count) {
        const uint64_t remaining =
            reader->logical_frame_count - reader->logical_frame_position;
        esp_err_t err = seismic_recorder_samples_reader_skip(
            read_at,
            read_context,
            reader,
            remaining
        );
        if (err != ESP_OK) {
            return err;
        }
    }

    return
        !reader->block_loaded &&
        reader->logical_frame_position == reader->logical_frame_count &&
        reader->next_block_offset_bytes <= reader->file_size_bytes &&
        (!reader->require_exact_file_size ||
         reader->next_block_offset_bytes == reader->file_size_bytes)
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}


esp_err_t seismic_recorder_samples_recover_file(
    FILE *file,
    uint64_t file_size,
    uint32_t expected_sample_rate_hz,
    uint16_t expected_channel_count,
    uint64_t *frame_count,
    uint64_t *target_size_bytes,
    uint16_t *format_version
)
{
    if (
        file == NULL ||
        frame_count == NULL ||
        target_size_bytes == NULL ||
        format_version == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame_count = 0U;
    *target_size_bytes = 0U;
    *format_version = 0U;
    if (file_size < sizeof(seismic_recorder_binary_header_t) || file_size > (uint64_t)LONG_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    seismic_recorder_binary_header_t header;
    if (
        fseek(file, 0L, SEEK_SET) != 0 ||
        fread(&header, 1U, sizeof(header), file) != sizeof(header)
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (
        header.version != SEISMIC_RECORDER_BINARY_VERSION_V1 &&
        header.version != SEISMIC_RECORDER_BINARY_VERSION_V2
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    seismic_recorder_binary_v2_metadata_t metadata = {0};
    uint64_t cursor = sizeof(header);
    if (header.version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
        if (file_size < cursor + sizeof(metadata)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (
            fread(&metadata, 1U, sizeof(metadata), file) != sizeof(metadata) ||
            !seismic_recorder_samples_metadata_valid(&metadata)
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cursor += sizeof(metadata);
    }
    if (!seismic_recorder_samples_header_valid(
            &header,
            &metadata,
            expected_sample_rate_hz,
            expected_channel_count
        )) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (header.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
        const uint64_t payload = file_size - cursor;
        const uint64_t frames = payload / sizeof(seismic_frame_t);
        uint64_t frame_bytes = 0U;
        if (
            !seismic_recorder_samples_mul_u64(frames, sizeof(seismic_frame_t), &frame_bytes) ||
            !seismic_recorder_samples_add_u64(cursor, frame_bytes, target_size_bytes)
        ) {
            return ESP_ERR_INVALID_SIZE;
        }
        *frame_count = frames;
        *format_version = header.version;
        return ESP_OK;
    }

    uint64_t total_frames = 0U;
    uint64_t previous_sample_index = 0U;
    uint64_t previous_timestamp_us = 0U;
    bool have_previous = false;
    while (cursor < file_size) {
        const uint64_t remaining = file_size - cursor;
        if (remaining < sizeof(seismic_recorder_binary_v2_block_header_t)) {
            break;
        }

        seismic_recorder_binary_v2_block_header_t block;
        if (
            cursor > (uint64_t)LONG_MAX ||
            fseek(file, (long)cursor, SEEK_SET) != 0 ||
            fread(&block, 1U, sizeof(block), file) != sizeof(block)
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (
            block.frame_count == 0U ||
            block.frame_count > SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES ||
            (block.flags & ~SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U ||
            block.first_timestamp_us > (uint64_t)INT64_MAX
        ) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (have_previous) {
            if (previous_sample_index == UINT64_MAX) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const uint64_t expected_index = previous_sample_index + 1U;
            const bool gap = (block.flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U;
            if (
                (gap && block.first_sample_index <= expected_index) ||
                (!gap && block.first_sample_index != expected_index) ||
                block.first_timestamp_us < previous_timestamp_us
            ) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
        else if ((block.flags & SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE) != 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        uint64_t payload_bytes = 0U;
        uint64_t payload_start = 0U;
        if (
            !seismic_recorder_samples_add_u64(cursor, sizeof(block), &payload_start) ||
            !seismic_recorder_samples_mul_u64(
                block.frame_count,
                SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
                &payload_bytes
            )
        ) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint64_t available = file_size - payload_start;
        if (available < payload_bytes) {
            const uint64_t complete_frames =
                available / SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES;
            if (
                block.first_sample_index > UINT64_MAX -
                    (complete_frames > 0U ? complete_frames - 1U : 0U)
            ) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (complete_frames > 0U) {
                block.frame_count = (uint16_t)complete_frames;
                if (
                    cursor > (uint64_t)LONG_MAX ||
                    fseek(file, (long)cursor, SEEK_SET) != 0 ||
                    fwrite(&block, sizeof(block), 1U, file) != 1U ||
                    fflush(file) != 0
                ) {
                    return ESP_FAIL;
                }
                uint64_t recovered_payload = 0U;
                if (
                    !seismic_recorder_samples_mul_u64(
                        complete_frames,
                        SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
                        &recovered_payload
                    ) ||
                    !seismic_recorder_samples_add_u64(
                        payload_start,
                        recovered_payload,
                        target_size_bytes
                    ) ||
                    !seismic_recorder_samples_add_u64(
                        total_frames,
                        complete_frames,
                        &total_frames
                    )
                ) {
                    return ESP_ERR_INVALID_SIZE;
                }
            }
            else {
                *target_size_bytes = cursor;
            }
            previous_sample_index = block.first_sample_index + complete_frames - (complete_frames > 0U ? 1U : 0U);
            if (complete_frames > 0U) {
                if (!seismic_recorder_samples_timestamp_at(
                        block.first_timestamp_us,
                        complete_frames - 1U,
                        header.sample_rate_hz,
                        &previous_timestamp_us
                    ) || previous_timestamp_us > (uint64_t)INT64_MAX) {
                    return ESP_ERR_INVALID_RESPONSE;
                }
            }
            have_previous = complete_frames > 0U || have_previous;
            goto recovered_tail;
        }

        uint64_t next_index = 0U;
        uint64_t last_timestamp = 0U;
        if (
            block.first_sample_index > UINT64_MAX - ((uint64_t)block.frame_count - 1U) ||
            !seismic_recorder_samples_add_u64(
                total_frames,
                block.frame_count,
                &total_frames
            ) ||
            !seismic_recorder_samples_timestamp_at(
                block.first_timestamp_us,
                (uint64_t)block.frame_count - 1U,
                header.sample_rate_hz,
                &last_timestamp
            ) ||
            last_timestamp > (uint64_t)INT64_MAX
        ) {
            return ESP_ERR_INVALID_SIZE;
        }
        next_index = block.first_sample_index + (uint64_t)block.frame_count - 1U;
        previous_sample_index = next_index;
        previous_timestamp_us = last_timestamp;
        have_previous = true;
        cursor = payload_start + payload_bytes;
        *target_size_bytes = cursor;
    }

    if (*target_size_bytes == 0U) {
        *target_size_bytes = cursor;
    }

recovered_tail:
    *frame_count = total_frames;
    *format_version = header.version;
    return ESP_OK;
}
