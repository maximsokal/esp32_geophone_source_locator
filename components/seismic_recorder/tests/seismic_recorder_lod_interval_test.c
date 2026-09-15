#include "seismic_recorder_lod_reader.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>


typedef struct
{
    const unsigned char *data;
    size_t size;

} memory_reader_t;


static esp_err_t memory_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    memory_reader_t *reader = (memory_reader_t *)context;
    if (
        reader == NULL || buffer == NULL || size_bytes == 0U ||
        offset_bytes > reader->size ||
        size_bytes > reader->size - (size_t)offset_bytes
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(buffer, reader->data + (size_t)offset_bytes, size_bytes);
    return ESP_OK;
}


static seismic_frame_t make_frame(size_t index)
{
    const int64_t gap_us = index >= 32U ? 6000LL : 0LL;
    seismic_frame_t frame = {
        .timestamp_us = 1000000LL + (int64_t)index * 2000LL + gap_us,
        .channel_uv = {
            (float)index,
            -(float)index,
            (float)index * 2.0f,
            -(float)index * 2.0f,
        },
    };
    return frame;
}


int main(void)
{
    seismic_frame_t frames[SEISMIC_RECORDER_LOD_BLOCK_FRAMES];
    for (size_t index = 0U; index < SEISMIC_RECORDER_LOD_BLOCK_FRAMES; ++index) {
        frames[index] = make_frame(index);
    }

    unsigned char raw_file[
        sizeof(seismic_recorder_binary_header_t) + sizeof(frames)
    ];
    seismic_recorder_binary_header_t raw_header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = sizeof(seismic_frame_t),
        .reserved = 0U,
    };
    memcpy(raw_file, &raw_header, sizeof(raw_header));
    memcpy(raw_file + sizeof(raw_header), frames, sizeof(frames));

    seismic_recorder_lod_record_t lod_record;
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);
    size_t record_count = 0U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        frames,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        &lod_record,
        1U,
        &record_count
    ) == ESP_OK);
    assert(record_count == 1U);

    unsigned char lod_file[
        sizeof(seismic_recorder_lod_file_header_t) +
        sizeof(seismic_recorder_lod_record_t)
    ];
    seismic_recorder_lod_file_header_t lod_header;
    seismic_recorder_lod_header_init(&lod_header, 500U);
    seismic_recorder_lod_header_mark_complete(&lod_header);
    memcpy(lod_file, &lod_header, sizeof(lod_header));
    memcpy(lod_file + sizeof(lod_header), &lod_record, sizeof(lod_record));

    seismic_recorder_session_summary_t summary = {0};
    summary.complete = true;
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.samples_written = SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
    snprintf(summary.session_id, sizeof(summary.session_id), "%s", "S000001");
    snprintf(summary.source_name, sizeof(summary.source_name), "%s", "AD7606");

    memory_reader_t raw_reader = {
        .data = raw_file,
        .size = sizeof(raw_file),
    };
    memory_reader_t lod_reader = {
        .data = lod_file,
        .size = sizeof(lod_file),
    };

    seismic_frame_t raw_staging[SEISMIC_RECORDER_LOD_BLOCK_FRAMES];
    seismic_recorder_lod_record_t lod_staging[1];
    seismic_recorder_waveform_point_t raw_point[1];
    seismic_recorder_waveform_point_t lod_point[1];
    seismic_recorder_waveform_info_t raw_info;
    seismic_recorder_waveform_info_t lod_info;
    seismic_recorder_lod_read_stats_t stats;

    assert(seismic_recorder_waveform_read_source(
        memory_read_at,
        &raw_reader,
        sizeof(raw_file),
        &summary,
        0U,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        1U,
        raw_staging,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        raw_point,
        1U,
        &raw_info
    ) == ESP_OK);

    assert(seismic_recorder_lod_read_source_with_stats(
        memory_read_at,
        &raw_reader,
        sizeof(raw_file),
        memory_read_at,
        &lod_reader,
        sizeof(lod_file),
        &summary,
        0U,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        1U,
        raw_staging,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        lod_staging,
        1U,
        lod_point,
        1U,
        &lod_info,
        &stats
    ) == ESP_OK);

    assert(raw_info.point_count == 1U);
    assert(lod_info.point_count == 1U);
    assert(raw_point[0].timestamp_us == frames[0].timestamp_us);
    assert(raw_point[0].end_timestamp_us == frames[63].timestamp_us);
    assert(raw_point[0].end_timestamp_us - raw_point[0].timestamp_us == 132000LL);
    assert(lod_point[0].timestamp_us == raw_point[0].timestamp_us);
    assert(lod_point[0].end_timestamp_us == raw_point[0].end_timestamp_us);

    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        assert(lod_point[0].min_uv[channel] == raw_point[0].min_uv[channel]);
        assert(lod_point[0].max_uv[channel] == raw_point[0].max_uv[channel]);
    }

    assert(stats.raw_frames_read == 0U);
    assert(stats.lod_records_read == 1U);
    assert(stats.lod_frames_read == SEISMIC_RECORDER_LOD_BLOCK_FRAMES);

    puts("seismic recorder LOD interval tests passed");
    return 0;
}
