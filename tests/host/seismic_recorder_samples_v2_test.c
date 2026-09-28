#include "seismic_recorder_internal.h"
#include "seismic_recorder_lod.h"
#include "seismic_recorder_lod_reader.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>


typedef struct
{
    FILE *file;
} file_context_t;


static esp_err_t read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    file_context_t *file_context = (file_context_t *)context;
    return
        file_context != NULL && file_context->file != NULL &&
        offset_bytes <= (uint64_t)LONG_MAX &&
        fseek(file_context->file, (long)offset_bytes, SEEK_SET) == 0 &&
        fread(buffer, 1U, size_bytes, file_context->file) == size_bytes
            ? ESP_OK
            : ESP_ERR_INVALID_RESPONSE;
}


static uint64_t file_size(FILE *file)
{
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size >= 0L);
    return (uint64_t)size;
}


static seismic_recorder_sample_format_t ad7606_format(void)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "AD7606",
        .sample_rate_hz = 500U,
        .bus_clock_hz = 10000000U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .oversampling_ratio = 4U,
        .input_range_v = 5.0f,
    };
    seismic_recorder_sample_format_t format;
    assert(seismic_recorder_samples_format_from_acquisition(&acquisition, &format) == ESP_OK);
    return format;
}


static void write_v2_prefix(
    FILE *file,
    const seismic_recorder_sample_format_t *format,
    uint32_t sample_rate_hz
)
{
    const seismic_recorder_binary_header_t header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION_V2,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = sample_rate_hz,
        .frame_size_bytes = SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES,
        .reserved = 0U,
    };
    seismic_recorder_binary_v2_metadata_t metadata;
    seismic_recorder_samples_v2_metadata_from_format(format, &metadata);
    assert(fseek(file, 0L, SEEK_SET) == 0);
    assert(fwrite(&header, sizeof(header), 1U, file) == 1U);
    assert(fwrite(&metadata, sizeof(metadata), 1U, file) == 1U);
}


static void write_v1_prefix(FILE *file, uint32_t sample_rate_hz)
{
    const seismic_recorder_binary_header_t header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION_V1,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = sample_rate_hz,
        .frame_size_bytes = sizeof(seismic_frame_t),
        .reserved = 0U,
    };
    assert(fseek(file, 0L, SEEK_SET) == 0);
    assert(fwrite(&header, sizeof(header), 1U, file) == 1U);
}


static size_t append_v2_block(
    FILE *file,
    const seismic_recorder_sample_format_t *format,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    bool gap_before
)
{
    uint8_t bytes[
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES *
            SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    ];
    size_t bytes_written = 0U;
    assert(seismic_recorder_samples_encode_v2_block(
        format,
        frames,
        sample_indices,
        frame_count,
        gap_before,
        bytes,
        sizeof(bytes),
        &bytes_written
    ) == ESP_OK);
    assert(fwrite(bytes, 1U, bytes_written, file) == bytes_written);
    return bytes_written;
}


static int16_t read_i16_le(const uint8_t *bytes)
{
    const uint16_t raw = (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
    int16_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}


static seismic_recorder_session_summary_t summary_for(
    uint16_t version,
    uint64_t frame_count,
    bool complete
)
{
    seismic_recorder_session_summary_t summary = {0};
    snprintf(summary.session_id, sizeof(summary.session_id), "S000001");
    snprintf(summary.source_name, sizeof(summary.source_name), "AD7606");
    summary.format_version = version;
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.samples_written = frame_count;
    summary.complete = complete;
    return summary;
}


static void test_ad7606_scale_adc_roundtrip_and_gap_timing(void)
{
    const seismic_recorder_sample_format_t format = ad7606_format();
    assert(format.version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(fabsf(format.scale_uv_per_lsb[0] - 152.587890625f) < 0.00001f);
    assert(format.oversampling_ratio == 4U && format.spi_clock_hz == 10000000U);

    const seismic_acquisition_snapshot_t simulator = {
        .source_name = "SIMULATOR",
        .sample_rate_hz = 500U,
    };
    seismic_recorder_sample_format_t simulator_format;
    assert(seismic_recorder_samples_format_from_acquisition(
        &simulator, &simulator_format
    ) == ESP_OK);
    assert(simulator_format.version == SEISMIC_RECORDER_BINARY_VERSION_V1);

    const int16_t raw_codes[4][SEISMIC_CHANNEL_COUNT] = {
        {-32768, -12345, 0, 32767},
        {-100, 1, 12345, 30000},
        {32767, 0, -1, -32768},
        {123, -456, 789, -1011},
    };
    const uint64_t indices[4] = {20U, 21U, 24U, 25U};
    seismic_frame_t frames[4] = {0};
    for (size_t frame = 0U; frame < 4U; ++frame) {
        frames[frame].timestamp_us = 500000LL + (int64_t)frame * 2000LL;
        if (frame >= 2U) {
            frames[frame].timestamp_us += 4000LL;
        }
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            frames[frame].channel_uv[channel] =
                (float)raw_codes[frame][channel] * format.scale_uv_per_lsb[channel];
        }
    }

    FILE *file = tmpfile();
    assert(file != NULL);
    write_v2_prefix(file, &format, 500U);
    assert(append_v2_block(file, &format, frames, indices, 2U, false) ==
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + 2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES);
    assert(append_v2_block(file, &format, &frames[2], &indices[2], 2U, true) ==
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + 2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES);
    assert(fflush(file) == 0);

    const uint64_t raw_offset = sizeof(seismic_recorder_binary_header_t) +
        sizeof(seismic_recorder_binary_v2_metadata_t) +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES;
    uint8_t payload[2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    file_context_t context = {.file = file};
    assert(read_at(&context, raw_offset, payload, sizeof(payload)) == ESP_OK);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const size_t offset =
                frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * 2U;
            assert(read_i16_le(&payload[offset]) == raw_codes[frame][channel]);
        }
    }

    seismic_recorder_samples_reader_t reader;
    seismic_recorder_session_summary_t summary = summary_for(2U, 4U, true);
    assert(seismic_recorder_samples_reader_open(
        read_at, &context, file_size(file), &summary, &reader
    ) == ESP_OK);
    seismic_frame_t decoded[4] = {0};
    size_t count = 0U;
    uint64_t first_index = 0U;
    bool gap_before = true;
    assert(seismic_recorder_samples_reader_next(
        read_at, &context, &reader, decoded, 4U, &count, &first_index, &gap_before
    ) == ESP_OK);
    assert(count == 2U && first_index == 20U && !gap_before);
    assert(decoded[0].timestamp_us == 500000LL && decoded[1].timestamp_us == 502000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(decoded[frame].channel_uv[channel] - frames[frame].channel_uv[channel]) <=
                format.scale_uv_per_lsb[channel] * 0.5f);
        }
    }
    assert(seismic_recorder_samples_reader_next(
        read_at, &context, &reader, decoded, 4U, &count, &first_index, &gap_before
    ) == ESP_OK);
    assert(count == 2U && first_index == 24U && gap_before);
    assert(decoded[0].timestamp_us == 508000LL && decoded[1].timestamp_us == 510000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(decoded[frame].channel_uv[channel] - frames[frame + 2U].channel_uv[channel]) <=
                format.scale_uv_per_lsb[channel] * 0.5f);
        }
    }
    assert(reader.has_gaps && reader.missing_sample_count == 2U);
    assert(seismic_recorder_samples_reader_finish(read_at, &context, &reader) == ESP_OK);

    seismic_frame_t staging[3] = {0};
    seismic_frame_t raw_frames[4] = {0};
    size_t raw_count = 0U;
    assert(seismic_recorder_waveform_read_raw_source(
        read_at, &context, file_size(file), &summary,
        0U, 0U, staging, 3U, raw_frames, 4U, &raw_count
    ) == ESP_OK);
    assert(raw_count == 4U);
    assert(raw_frames[2].timestamp_us == 508000LL);
    assert(fabsf(raw_frames[3].channel_uv[3] - frames[3].channel_uv[3]) <=
        format.scale_uv_per_lsb[3] * 0.5f);
    assert(fclose(file) == 0);
}


static void test_clamping_and_invalid_headers(void)
{
    const seismic_recorder_sample_format_t format = ad7606_format();
    const uint64_t index = 1U;
    seismic_frame_t frame = {
        .timestamp_us = 1000LL,
        .channel_uv = {-1000000000.0f, 1000000000.0f, 32767.0f * 152.587890625f,
                       -32768.0f * 152.587890625f},
    };
    uint8_t encoded[SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    size_t bytes_written = 0U;
    assert(seismic_recorder_samples_encode_v2_block(
        &format, &frame, &index, 1U, false, encoded, sizeof(encoded), &bytes_written
    ) == ESP_OK);
    assert(bytes_written == sizeof(encoded));
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        const int16_t raw = read_i16_le(
            encoded + SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES + channel * 2U
        );
        assert(raw == (channel == 0U || channel == 3U ? INT16_MIN : INT16_MAX));
    }
    const uint64_t overflowing_indices[2] = {UINT64_MAX, 0U};
    const seismic_frame_t index_frames[2] = {frame, frame};
    uint8_t index_encoded[
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    ];
    assert(seismic_recorder_samples_encode_v2_block(
        &format, index_frames, overflowing_indices, 2U, false,
        index_encoded, sizeof(index_encoded), &bytes_written
    ) == ESP_ERR_INVALID_ARG);

    FILE *bad_version = tmpfile();
    assert(bad_version != NULL);
    seismic_recorder_binary_header_t header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = 3U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = 8U,
    };
    assert(fwrite(&header, sizeof(header), 1U, bad_version) == 1U);
    assert(fflush(bad_version) == 0);
    file_context_t context = {.file = bad_version};
    seismic_recorder_samples_reader_t reader;
    size_t count = 0U;
    seismic_recorder_session_summary_t summary = summary_for(3U, 0U, true);
    assert(seismic_recorder_samples_reader_open(
        read_at, &context, file_size(bad_version), &summary, &reader
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(bad_version) == 0);

    FILE *zero_scale = tmpfile();
    assert(zero_scale != NULL);
    seismic_recorder_sample_format_t zero_format = format;
    zero_format.scale_uv_per_lsb[2] = 0.0f;
    write_v2_prefix(zero_scale, &zero_format, 500U);
    assert(fflush(zero_scale) == 0);
    context.file = zero_scale;
    summary = summary_for(2U, 0U, true);
    assert(seismic_recorder_samples_reader_open(
        read_at, &context, file_size(zero_scale), &summary, &reader
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(zero_scale) == 0);

    FILE *bad_block = tmpfile();
    assert(bad_block != NULL);
    write_v2_prefix(bad_block, &format, 500U);
    const seismic_recorder_binary_v2_block_header_t invalid_block = {
        .first_sample_index = 1U,
        .first_timestamp_us = 1000U,
        .frame_count = 0U,
        .flags = 0U,
    };
    assert(fwrite(&invalid_block, sizeof(invalid_block), 1U, bad_block) == 1U);
    assert(fflush(bad_block) == 0);
    context.file = bad_block;
    summary = summary_for(2U, 1U, true);
    assert(seismic_recorder_samples_reader_open(
        read_at, &context, file_size(bad_block), &summary, &reader
    ) == ESP_OK);
    assert(seismic_recorder_samples_reader_next(
        read_at, &context, &reader, &frame, 1U, &count, NULL, NULL
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(bad_block) == 0);
}


static void test_recovery_keeps_complete_frames_of_truncated_block(void)
{
    const seismic_recorder_sample_format_t format = ad7606_format();
    FILE *file = tmpfile();
    assert(file != NULL);
    write_v2_prefix(file, &format, 500U);
    seismic_frame_t frames[3] = {
        {.timestamp_us = 10000LL, .channel_uv = {1.0f, 2.0f, 3.0f, 4.0f}},
        {.timestamp_us = 12000LL, .channel_uv = {5.0f, 6.0f, 7.0f, 8.0f}},
        {.timestamp_us = 14000LL, .channel_uv = {9.0f, 10.0f, 11.0f, 12.0f}},
    };
    const uint64_t indices[3] = {50U, 51U, 52U};
    uint8_t encoded[SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        3U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    size_t bytes_written = 0U;
    assert(seismic_recorder_samples_encode_v2_block(
        &format, frames, indices, 3U, false, encoded, sizeof(encoded), &bytes_written
    ) == ESP_OK);
    assert(bytes_written == sizeof(encoded));
    const size_t partial_size = sizeof(encoded) -
        SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + 1U;
    assert(fwrite(encoded, 1U, partial_size, file) == partial_size);
    assert(fflush(file) == 0);

    uint64_t recovered_frames = 0U;
    uint64_t target_size = 0U;
    uint16_t version = 0U;
    assert(seismic_recorder_samples_recover_file(
        file,
        file_size(file),
        500U,
        SEISMIC_CHANNEL_COUNT,
        &recovered_frames,
        &target_size,
        &version
    ) == ESP_OK);
    assert(recovered_frames == 2U);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(target_size == sizeof(seismic_recorder_binary_header_t) +
        sizeof(seismic_recorder_binary_v2_metadata_t) +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES);
    seismic_recorder_binary_v2_block_header_t recovered_block;
    assert(read_at(&(file_context_t){.file = file},
        sizeof(seismic_recorder_binary_header_t) +
            sizeof(seismic_recorder_binary_v2_metadata_t),
        &recovered_block, sizeof(recovered_block)) == ESP_OK);
    assert(recovered_block.frame_count == 2U);

    seismic_recorder_session_summary_t summary = summary_for(2U, recovered_frames, false);
    summary.recovered = true;
    file_context_t context = {.file = file};
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        read_at, &context, target_size, &summary, &reader
    ) == ESP_OK);
    seismic_frame_t decoded[2] = {0};
    size_t count = 0U;
    assert(seismic_recorder_samples_reader_next(
        read_at, &context, &reader, decoded, 2U, &count, NULL, NULL
    ) == ESP_OK);
    assert(count == 2U && decoded[1].timestamp_us == 12000LL);
    assert(seismic_recorder_samples_reader_finish(read_at, &context, &reader) == ESP_OK);
    assert(fclose(file) == 0);
}


static void test_lod_reads_quantized_v2_and_v1_remains_readable(void)
{
    const seismic_recorder_sample_format_t format = ad7606_format();
    seismic_frame_t frames[SEISMIC_RECORDER_LOD_BLOCK_FRAMES] = {0};
    seismic_frame_t quantized[SEISMIC_RECORDER_LOD_BLOCK_FRAMES] = {0};
    uint64_t indices[SEISMIC_RECORDER_LOD_BLOCK_FRAMES];
    for (size_t frame = 0U; frame < SEISMIC_RECORDER_LOD_BLOCK_FRAMES; ++frame) {
        indices[frame] = (uint64_t)frame + 1U;
        frames[frame].timestamp_us = 1000000LL + (int64_t)frame * 2000LL;
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            frames[frame].channel_uv[channel] =
                ((float)((int)frame - 32) * (float)(channel + 1U) + 0.25f) *
                format.scale_uv_per_lsb[channel];
        }
        assert(seismic_recorder_samples_quantize_frame(
            &format, &frames[frame], &quantized[frame]
        ) == ESP_OK);
    }

    FILE *raw_file = tmpfile();
    FILE *lod_file = tmpfile();
    assert(raw_file != NULL && lod_file != NULL);
    write_v2_prefix(raw_file, &format, 500U);
    assert(append_v2_block(
        raw_file, &format, frames, indices,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES, false
    ) > 0U);
    assert(fflush(raw_file) == 0);

    seismic_recorder_lod_file_header_t lod_header;
    seismic_recorder_lod_header_init(&lod_header, 500U);
    seismic_recorder_lod_header_mark_complete(&lod_header);
    seismic_recorder_lod_builder_t builder = {0};
    seismic_recorder_lod_builder_reset(&builder, 0U);
    seismic_recorder_lod_record_t record;
    size_t record_count = 0U;
    assert(seismic_recorder_lod_builder_append(
        &builder, 0U, quantized, SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        &record, 1U, &record_count
    ) == ESP_OK);
    assert(record_count == 1U);
    assert(fwrite(&lod_header, sizeof(lod_header), 1U, lod_file) == 1U);
    assert(fwrite(&record, sizeof(record), 1U, lod_file) == 1U);
    assert(fflush(lod_file) == 0);

    file_context_t raw_context = {.file = raw_file};
    file_context_t lod_context = {.file = lod_file};
    seismic_recorder_session_summary_t summary = summary_for(2U, 64U, true);
    seismic_frame_t raw_staging[64] = {0};
    seismic_recorder_lod_record_t lod_staging[1] = {0};
    seismic_recorder_waveform_point_t points[1] = {0};
    seismic_recorder_waveform_info_t info;
    seismic_recorder_lod_read_stats_t stats;
    assert(seismic_recorder_lod_read_source_with_stats(
        read_at, &raw_context, file_size(raw_file),
        read_at, &lod_context, file_size(lod_file),
        &summary, 0U, 64U, 1U,
        raw_staging, 64U, lod_staging, 1U,
        points, 1U, &info, &stats
    ) == ESP_OK);
    assert(info.format_version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(info.point_count == 1U && stats.lod_records_read == 1U);
    for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
        float expected_min = quantized[0].channel_uv[channel];
        float expected_max = expected_min;
        for (size_t frame = 1U; frame < SEISMIC_RECORDER_LOD_BLOCK_FRAMES; ++frame) {
            if (quantized[frame].channel_uv[channel] < expected_min) {
                expected_min = quantized[frame].channel_uv[channel];
            }
            if (quantized[frame].channel_uv[channel] > expected_max) {
                expected_max = quantized[frame].channel_uv[channel];
            }
        }
        assert(points[0].min_uv[channel] == expected_min);
        assert(points[0].max_uv[channel] == expected_max);
    }

    FILE *v1_file = tmpfile();
    assert(v1_file != NULL);
    write_v1_prefix(v1_file, 500U);
    const seismic_frame_t v1_frame = {
        .timestamp_us = 2000LL,
        .channel_uv = {1.0f, -2.0f, 3.0f, -4.0f},
    };
    assert(fwrite(&v1_frame, sizeof(v1_frame), 1U, v1_file) == 1U);
    assert(fflush(v1_file) == 0);
    file_context_t v1_context = {.file = v1_file};
    seismic_recorder_samples_reader_t v1_reader;
    seismic_recorder_session_summary_t v1_summary = summary_for(1U, 1U, true);
    assert(seismic_recorder_samples_reader_open(
        read_at, &v1_context, file_size(v1_file), &v1_summary, &v1_reader
    ) == ESP_OK);
    assert(seismic_recorder_samples_reader_next(
        read_at, &v1_context, &v1_reader, raw_staging, 1U,
        &record_count, NULL, NULL
    ) == ESP_OK);
    assert(record_count == 1U && raw_staging[0].timestamp_us == 2000LL);
    assert(raw_staging[0].channel_uv[1] == -2.0f);

    assert(fclose(v1_file) == 0);
    assert(fclose(lod_file) == 0);
    assert(fclose(raw_file) == 0);
}


static void test_timestamp_math_at_40khz_for_24_hours(void)
{
    const uint64_t samples_24h = 24ULL * 60ULL * 60ULL * 40000ULL;
    uint64_t timestamp_us = 0U;
    assert(seismic_recorder_samples_timestamp_at(
        1234U, samples_24h, 40000U, &timestamp_us
    ));
    assert(timestamp_us == 1234U + 24ULL * 60ULL * 60ULL * 1000000ULL);
    assert(!seismic_recorder_samples_timestamp_at(0U, 1U, 0U, &timestamp_us));
    assert(!seismic_recorder_samples_timestamp_at(0U, UINT64_MAX, 1U, &timestamp_us));
    assert(!seismic_recorder_samples_timestamp_at(
        UINT64_MAX - 1U, 1U, 500U, &timestamp_us
    ));
}


int main(void)
{
    test_ad7606_scale_adc_roundtrip_and_gap_timing();
    test_clamping_and_invalid_headers();
    test_recovery_keeps_complete_frames_of_truncated_block();
    test_lod_reads_quantized_v2_and_v1_remains_readable();
    test_timestamp_math_at_40khz_for_24_hours();
    puts("seismic recorder samples v2 tests passed");
    return 0;
}
