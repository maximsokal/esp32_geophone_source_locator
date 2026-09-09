#include "seismic_recorder_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>


static seismic_recorder_binary_header_t valid_header(void)
{
    return (seismic_recorder_binary_header_t) {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = sizeof(seismic_frame_t),
        .reserved = 0U,
    };
}


static seismic_recorder_session_summary_t valid_summary(void)
{
    seismic_recorder_session_summary_t summary = {0};
    snprintf(summary.session_id, sizeof(summary.session_id), "S000001");
    snprintf(summary.source_name, sizeof(summary.source_name), "AD7606");
    summary.complete = true;
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.samples_written = 10U;
    summary.recording_ms = 20U;
    return summary;
}


static size_t write_frames(
    FILE *file,
    const seismic_recorder_binary_header_t *header,
    size_t frame_count
)
{
    assert(fseek(file, 0L, SEEK_SET) == 0);
    assert(fwrite(header, sizeof(*header), 1U, file) == 1U);

    for (size_t index = 0U; index < frame_count; ++index) {
        seismic_frame_t frame = {
            .timestamp_us = (int64_t)(index + 1U) * 2000LL,
            .channel_uv = {
                (float)index,
                (float)index + 10.0f,
                (float)index + 20.0f,
                (float)index + 30.0f,
            },
        };
        if (index == 6U) {
            frame.channel_uv[0] = 18000.0f;
            frame.channel_uv[1] = -12000.0f;
        }
        assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    }
    assert(fflush(file) == 0);
    return sizeof(*header) + frame_count * sizeof(seismic_frame_t);
}


static void test_valid_range_envelope_and_spike(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 10U);
    const seismic_recorder_session_summary_t summary = valid_summary();
    seismic_recorder_waveform_point_t points[4] = {0};
    seismic_recorder_waveform_info_t info;

    assert(seismic_recorder_waveform_read_file(
        file,
        file_size,
        &summary,
        0U,
        0U,
        4U,
        points,
        4U,
        &info
    ) == ESP_OK);
    assert(info.source_frame_count == 10U);
    assert(info.start_frame == 0U && info.frame_count == 10U);
    assert(info.point_count == 4U);
    assert(info.start_timestamp_us == 2000LL);
    assert(info.end_timestamp_us == 20000LL);

    bool saw_positive_spike = false;
    bool saw_negative_spike = false;
    for (size_t point = 0U; point < info.point_count; ++point) {
        saw_positive_spike = saw_positive_spike || points[point].max_uv[0] == 18000.0f;
        saw_negative_spike = saw_negative_spike || points[point].min_uv[1] == -12000.0f;
    }
    assert(saw_positive_spike && saw_negative_spike);

    assert(seismic_recorder_waveform_read_file(
        file,
        file_size,
        &summary,
        2U,
        4U,
        4U,
        points,
        4U,
        &info
    ) == ESP_OK);
    assert(info.source_frame_count == 10U);
    assert(info.start_frame == 2U && info.frame_count == 4U);
    assert(info.point_count == 4U);
    assert(info.start_timestamp_us == 6000LL);
    assert(info.end_timestamp_us == 12000LL);

    assert(fclose(file) == 0);
}


static void test_bad_headers_and_truncation(void)
{
    const seismic_recorder_session_summary_t summary = valid_summary();
    seismic_recorder_waveform_point_t points[4] = {0};
    seismic_recorder_waveform_info_t info;

    for (unsigned failure = 0U; failure < 4U; ++failure) {
        FILE *file = tmpfile();
        assert(file != NULL);
        seismic_recorder_binary_header_t header = valid_header();
        if (failure == 0U) header.magic[0] = 'X';
        if (failure == 1U) header.version++;
        if (failure == 2U) header.channel_count = 8U;
        if (failure == 3U) header.frame_size_bytes++;
        const size_t file_size = write_frames(file, &header, 1U);
        assert(seismic_recorder_waveform_read_file(
            file, file_size, &summary, 0U, 0U, 4U, points, 4U, &info
        ) == ESP_ERR_INVALID_RESPONSE);
        assert(fclose(file) == 0);
    }

    FILE *truncated = tmpfile();
    assert(truncated != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    assert(write_frames(truncated, &header, 1U) == sizeof(header) + sizeof(seismic_frame_t));
    assert(seismic_recorder_waveform_read_file(
        truncated,
        sizeof(header) + sizeof(seismic_frame_t) - 1U,
        &summary,
        0U,
        0U,
        4U,
        points,
        4U,
        &info
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(truncated) == 0);
}


static void test_range_and_capacity_bounds(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 3U);
    const seismic_recorder_session_summary_t summary = valid_summary();
    seismic_recorder_waveform_point_t points[2] = {0};
    seismic_recorder_waveform_info_t info;

    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 4U, 0U, 2U, points, 2U, &info
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 2U, 2U, 2U, points, 2U, &info
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 0U, 1U, 3U, points, 2U, &info
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 0U, 1U, 0U, points, 2U, &info
    ) == ESP_ERR_INVALID_ARG);

    assert(fclose(file) == 0);
}


int main(void)
{
    test_valid_range_envelope_and_spike();
    test_bad_headers_and_truncation();
    test_range_and_capacity_bounds();
    puts("seismic recorder waveform tests passed");
    return 0;
}
