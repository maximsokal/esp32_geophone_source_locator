#include "seismic_recorder_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>


typedef struct
{
    const uint8_t *bytes;
    size_t size;
} memory_reader_t;


static esp_err_t read_memory_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    const memory_reader_t *reader = (const memory_reader_t *)context;
    if (
        reader == NULL || buffer == NULL || offset_bytes > reader->size ||
        size_bytes > reader->size - (size_t)offset_bytes
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(buffer, reader->bytes + (size_t)offset_bytes, size_bytes);
    return ESP_OK;
}


static void append_block(
    uint8_t *file,
    size_t file_capacity,
    size_t *file_size,
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frames,
    const uint64_t *indices,
    bool gap_before
)
{
    uint8_t block[
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    ];
    size_t block_size = 0U;
    assert(seismic_recorder_samples_encode_v2_block(
        format, frames, indices, 2U, gap_before, block, sizeof(block), &block_size
    ) == ESP_OK);
    assert(block_size <= file_capacity - *file_size);
    memcpy(file + *file_size, block, block_size);
    *file_size += block_size;
}


int main(void)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "AD7606",
        .sample_rate_hz = 500U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .input_range_v = 5.0f,
    };
    seismic_recorder_sample_format_t format;
    assert(seismic_recorder_samples_format_from_acquisition(&acquisition, &format) == ESP_OK);

    const int16_t raw_codes[4][SEISMIC_CHANNEL_COUNT] = {
        { INT16_MAX, INT16_MIN, INT16_MAX, 10 },
        { INT16_MAX, INT16_MIN, 0, 20 },
        { INT16_MAX, INT16_MIN, INT16_MIN, 30 },
        { INT16_MAX, INT16_MIN, 0, 40 },
    };
    seismic_frame_t frames[4] = { 0 };
    const float scale = format.scale_uv_per_lsb[0];
    for (size_t frame = 0U; frame < 4U; ++frame) {
        frames[frame].timestamp_us = 100000LL + (int64_t)frame * 2000LL;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            frames[frame].channel_uv[channel] = (float)raw_codes[frame][channel] * scale;
        }
    }

    uint8_t file_bytes[
        sizeof(seismic_recorder_binary_header_t) +
        sizeof(seismic_recorder_binary_v2_metadata_t) +
        2U * (SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
              2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES)
    ] = { 0 };
    size_t file_size = 0U;
    const seismic_recorder_binary_header_t header = {
        .magic = { 'G', 'E', 'O', 'S', 'E', 'I', 'S', '1' },
        .version = SEISMIC_RECORDER_BINARY_VERSION_V2,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
    };
    seismic_recorder_binary_v2_metadata_t metadata;
    seismic_recorder_samples_v2_metadata_from_format(&format, &metadata);
    memcpy(file_bytes + file_size, &header, sizeof(header));
    file_size += sizeof(header);
    memcpy(file_bytes + file_size, &metadata, sizeof(metadata));
    file_size += sizeof(metadata);

    const uint64_t first_indices[2] = { 10U, 11U };
    const uint64_t second_indices[2] = { 14U, 15U };
    append_block(file_bytes, sizeof(file_bytes), &file_size, &format, frames, first_indices, false);
    append_block(file_bytes, sizeof(file_bytes), &file_size, &format, &frames[2], second_indices, true);

    const memory_reader_t memory = { .bytes = file_bytes, .size = file_size };
    const seismic_recorder_session_summary_t summary = {
        .complete = true,
        .readable = true,
        .sample_rate_hz = 500U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .samples_written = 4U,
        .format_version = SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        read_memory_at, (void *)&memory, file_size, &summary, &reader
    ) == ESP_OK);

    seismic_frame_t decoded[2] = { 0 };
    size_t frame_count = 0U;
    uint64_t first_index = 0U;
    bool gap_before = true;
    size_t rail_counts[SEISMIC_CHANNEL_COUNT] = { 0U };
    size_t samples_examined = 0U;
    assert(seismic_recorder_samples_reader_next_ex(
        read_memory_at, (void *)&memory, &reader, decoded, 2U, &frame_count,
        &first_index, &gap_before, rail_counts, &samples_examined
    ) == ESP_OK);
    assert(frame_count == 2U && first_index == 10U && !gap_before);
    assert(samples_examined == 2U);
    assert(rail_counts[0] == 2U && rail_counts[1] == 2U);
    assert(rail_counts[2] == 1U && rail_counts[3] == 0U);
    const seismic_frame_t first_block_decoded[2] = { decoded[0], decoded[1] };

    assert(seismic_recorder_samples_reader_next_ex(
        read_memory_at, (void *)&memory, &reader, decoded, 2U, &frame_count,
        &first_index, &gap_before, rail_counts, &samples_examined
    ) == ESP_OK);
    assert(frame_count == 2U && first_index == 14U && gap_before);
    assert(samples_examined == 2U);
    assert(rail_counts[0] == 2U && rail_counts[1] == 2U);
    assert(rail_counts[2] == 1U && rail_counts[3] == 0U);
    assert(seismic_recorder_samples_reader_finish(read_memory_at, (void *)&memory, &reader) == ESP_OK);

    /* The legacy entry still decodes the same values, indexes and gap flags. */
    assert(seismic_recorder_samples_reader_open(
        read_memory_at, (void *)&memory, file_size, &summary, &reader
    ) == ESP_OK);
    assert(seismic_recorder_samples_reader_next(
        read_memory_at, (void *)&memory, &reader, decoded, 2U, &frame_count,
        &first_index, &gap_before
    ) == ESP_OK);
    assert(frame_count == 2U && first_index == 10U && !gap_before);
    for (size_t i = 0U; i < 2U; ++i) {
        assert(decoded[i].timestamp_us == first_block_decoded[i].timestamp_us);
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(decoded[i].channel_uv[channel] == first_block_decoded[i].channel_uv[channel]);
        }
    }
    assert(seismic_recorder_samples_reader_next(
        read_memory_at, (void *)&memory, &reader, decoded, 2U, &frame_count,
        &first_index, &gap_before
    ) == ESP_OK);
    assert(frame_count == 2U && first_index == 14U && gap_before);
    assert(seismic_recorder_samples_reader_finish(read_memory_at, (void *)&memory, &reader) == ESP_OK);

    puts("seismic recorder raw rails: pinned-high=4 pinned-low=4 mixed=2 none=0 gap=preserved legacy=unchanged");
    return 0;
}
