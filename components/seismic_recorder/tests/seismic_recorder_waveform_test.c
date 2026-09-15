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
    summary.cadence.available = true;
    summary.cadence.timer_ticks = 10U;
    summary.cadence.conversion_attempts = 10U;
    summary.cadence.collapsed_periods = 0U;
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
    assert(info.sample_rate_hz == summary.sample_rate_hz);
    assert(info.channel_count == summary.channel_count);
    assert(info.start_timestamp_us == 2000LL);
    assert(info.end_timestamp_us == 20000LL);
    assert(info.cadence.available);
    assert(info.cadence.timer_ticks == 10U);
    assert(info.cadence.conversion_attempts == 10U);
    assert(info.cadence.collapsed_periods == 0U);

    assert(points[0].timestamp_us == 2000LL);
    assert(points[0].end_timestamp_us == 6000LL);
    assert(points[1].timestamp_us == 8000LL);
    assert(points[1].end_timestamp_us == 10000LL);
    assert(points[2].timestamp_us == 12000LL);
    assert(points[2].end_timestamp_us == 16000LL);
    assert(points[3].timestamp_us == 18000LL);
    assert(points[3].end_timestamp_us == 20000LL);

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
    for (size_t point = 0U; point < info.point_count; ++point) {
        assert(points[point].timestamp_us == points[point].end_timestamp_us);
    }

    assert(fclose(file) == 0);
}


static void test_non_monotonic_timestamps_are_rejected(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 3U);
    const long second_frame_offset =
        (long)sizeof(header) + (long)sizeof(seismic_frame_t);
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    seismic_frame_t frame;
    assert(fread(&frame, sizeof(frame), 1U, file) == 1U);
    frame.timestamp_us = 1000LL;
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    assert(fflush(file) == 0);

    seismic_recorder_session_summary_t summary = valid_summary();
    summary.samples_written = 3U;
    seismic_recorder_waveform_point_t points[3] = {0};
    seismic_recorder_waveform_info_t info;
    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 0U, 0U, 3U, points, 3U, &info
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(file) == 0);
}


static void test_metadata_raw_frame_count_mismatch(void)
{
    const seismic_recorder_binary_header_t header = valid_header();
    const seismic_recorder_session_summary_t summary = valid_summary();
    seismic_recorder_waveform_point_t points[4] = {0};
    seismic_recorder_waveform_info_t info;

    const size_t physical_counts[] = {9U, 11U};
    for (size_t index = 0U; index < 2U; ++index) {
        FILE *file = tmpfile();
        assert(file != NULL);
        const size_t file_size = write_frames(
            file,
            &header,
            physical_counts[index]
        );

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
        ) == ESP_ERR_INVALID_RESPONSE);

        assert(seismic_recorder_waveform_read_file(
            file,
            file_size,
            &summary,
            0U,
            4U,
            4U,
            points,
            4U,
            &info
        ) == ESP_ERR_INVALID_RESPONSE);

        assert(fclose(file) == 0);
    }
}


static void test_header_metadata_configuration_mismatch(void)
{
    seismic_recorder_waveform_point_t points[4] = {0};
    seismic_recorder_waveform_info_t info;

    FILE *rate_file = tmpfile();
    assert(rate_file != NULL);
    seismic_recorder_binary_header_t header = valid_header();
    header.sample_rate_hz = 1000U;
    const size_t rate_file_size = write_frames(rate_file, &header, 10U);
    const seismic_recorder_session_summary_t summary = valid_summary();
    assert(seismic_recorder_waveform_read_file(
        rate_file,
        rate_file_size,
        &summary,
        0U,
        0U,
        4U,
        points,
        4U,
        &info
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(rate_file) == 0);

    FILE *channel_file = tmpfile();
    assert(channel_file != NULL);
    header = valid_header();
    const size_t channel_file_size = write_frames(channel_file, &header, 10U);
    seismic_recorder_session_summary_t channel_summary = valid_summary();
    channel_summary.channel_count = SEISMIC_CHANNEL_COUNT - 1U;
    assert(seismic_recorder_waveform_read_file(
        channel_file,
        channel_file_size,
        &channel_summary,
        0U,
        0U,
        4U,
        points,
        4U,
        &info
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(fclose(channel_file) == 0);
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
        const size_t file_size = write_frames(file, &header, 10U);
        assert(seismic_recorder_waveform_read_file(
            file, file_size, &summary, 0U, 0U, 4U, points, 4U, &info
        ) == ESP_ERR_INVALID_RESPONSE);
        assert(fclose(file) == 0);
    }

    FILE *truncated = tmpfile();
    assert(truncated != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t exact_size = write_frames(truncated, &header, 10U);
    assert(seismic_recorder_waveform_read_file(
        truncated,
        exact_size - 1U,
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
    seismic_recorder_session_summary_t summary = valid_summary();
    summary.samples_written = 3U;
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

    assert(seismic_recorder_waveform_read_file(
        file, file_size, &summary, 3U, 0U, 2U, points, 2U, &info
    ) == ESP_OK);
    assert(info.source_frame_count == 3U);
    assert(info.start_frame == 3U);
    assert(info.frame_count == 0U);
    assert(info.point_count == 0U);

    assert(fclose(file) == 0);
}


int main(void)
{
    test_valid_range_envelope_and_spike();
    test_non_monotonic_timestamps_are_rejected();
    test_metadata_raw_frame_count_mismatch();
    test_header_metadata_configuration_mismatch();
    test_bad_headers_and_truncation();
    test_range_and_capacity_bounds();
    puts("seismic recorder waveform tests passed");
    return 0;
}
