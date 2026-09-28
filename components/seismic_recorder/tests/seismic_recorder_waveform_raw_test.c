#include "seismic_recorder_internal.h"
#include "../seismic_recorder_samples.c"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>


typedef struct
{
    FILE *file;
} raw_test_file_context_t;


static esp_err_t raw_test_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    raw_test_file_context_t *file_context = (raw_test_file_context_t *)context;
    if (fseek(file_context->file, (long)offset_bytes, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    return fread(buffer, 1U, size_bytes, file_context->file) == size_bytes
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}


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


static seismic_recorder_session_summary_t valid_summary(size_t samples_written)
{
    seismic_recorder_session_summary_t summary = {0};
    snprintf(summary.session_id, sizeof(summary.session_id), "S000001");
    snprintf(summary.source_name, sizeof(summary.source_name), "AD7606");
    summary.complete = true;
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.samples_written = samples_written;
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
        assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    }
    assert(fflush(file) == 0);
    return sizeof(*header) + frame_count * sizeof(seismic_frame_t);
}


static void test_full_session_preserves_exact_samples(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 10U);
    const seismic_recorder_session_summary_t summary = valid_summary(10U);
    raw_test_file_context_t context = { .file = file };
    seismic_frame_t staging[4];
    seismic_frame_t frames[10];
    size_t frame_count_out = 0U;

    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        0U, 0U, staging, 4U, frames, 10U, &frame_count_out
    ) == ESP_OK);
    assert(frame_count_out == 10U);

    for (size_t index = 0U; index < 10U; ++index) {
        assert(frames[index].timestamp_us == (int64_t)(index + 1U) * 2000LL);
        assert(frames[index].channel_uv[0] == (float)index);
        assert(frames[index].channel_uv[1] == (float)index + 10.0f);
        assert(frames[index].channel_uv[2] == (float)index + 20.0f);
        assert(frames[index].channel_uv[3] == (float)index + 30.0f);
    }

    assert(fclose(file) == 0);
}


static void test_partial_range_and_staging_smaller_than_range(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 10U);
    const seismic_recorder_session_summary_t summary = valid_summary(10U);
    raw_test_file_context_t context = { .file = file };
    /* Staging capacity (2) smaller than the requested range (5) forces the
     * chunked-read loop to run multiple iterations, exactly like a real
     * bounded storage-arbitration read would. */
    seismic_frame_t staging[2];
    seismic_frame_t frames[5];
    size_t frame_count_out = 0U;

    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        3U, 5U, staging, 2U, frames, 5U, &frame_count_out
    ) == ESP_OK);
    assert(frame_count_out == 5U);
    for (size_t index = 0U; index < 5U; ++index) {
        const size_t source_index = index + 3U;
        assert(frames[index].timestamp_us == (int64_t)(source_index + 1U) * 2000LL);
        assert(frames[index].channel_uv[0] == (float)source_index);
    }

    assert(fclose(file) == 0);
}


static void test_capacity_too_small_fails_closed_without_truncating(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 10U);
    const seismic_recorder_session_summary_t summary = valid_summary(10U);
    raw_test_file_context_t context = { .file = file };
    seismic_frame_t staging[4];
    seismic_frame_t frames[4];
    size_t frame_count_out = 99U;

    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        0U, 0U, staging, 4U, frames, 4U, &frame_count_out
    ) == ESP_ERR_INVALID_SIZE);
    /* Never silently returns a truncated prefix as if it were the whole
     * session -- frame_count_out is reset to 0 by the callee on entry. */
    assert(frame_count_out == 0U);

    assert(fclose(file) == 0);
}


static void test_non_monotonic_timestamps_are_rejected(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    write_frames(file, &header, 3U);
    const long second_frame_offset =
        (long)sizeof(header) + (long)sizeof(seismic_frame_t);
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    seismic_frame_t frame;
    assert(fread(&frame, sizeof(frame), 1U, file) == 1U);
    frame.timestamp_us = 1000LL;
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    assert(fflush(file) == 0);
    const size_t file_size = sizeof(header) + 3U * sizeof(seismic_frame_t);

    const seismic_recorder_session_summary_t summary = valid_summary(3U);
    raw_test_file_context_t context = { .file = file };
    seismic_frame_t staging[3];
    seismic_frame_t frames[3];
    size_t frame_count_out = 0U;

    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        0U, 0U, staging, 3U, frames, 3U, &frame_count_out
    ) == ESP_ERR_INVALID_RESPONSE);

    assert(fclose(file) == 0);
}


static void test_non_finite_sample_is_rejected(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    write_frames(file, &header, 3U);
    const long second_frame_offset =
        (long)sizeof(header) + (long)sizeof(seismic_frame_t);
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    seismic_frame_t frame;
    assert(fread(&frame, sizeof(frame), 1U, file) == 1U);
    frame.channel_uv[2] = INFINITY;
    assert(fseek(file, second_frame_offset, SEEK_SET) == 0);
    assert(fwrite(&frame, sizeof(frame), 1U, file) == 1U);
    assert(fflush(file) == 0);
    const size_t file_size = sizeof(header) + 3U * sizeof(seismic_frame_t);

    const seismic_recorder_session_summary_t summary = valid_summary(3U);
    raw_test_file_context_t context = { .file = file };
    seismic_frame_t staging[3];
    seismic_frame_t frames[3];
    size_t frame_count_out = 0U;

    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        0U, 0U, staging, 3U, frames, 3U, &frame_count_out
    ) == ESP_ERR_INVALID_RESPONSE);

    assert(fclose(file) == 0);
}


static void test_range_argument_validation(void)
{
    FILE *file = tmpfile();
    assert(file != NULL);
    const seismic_recorder_binary_header_t header = valid_header();
    const size_t file_size = write_frames(file, &header, 3U);
    const seismic_recorder_session_summary_t summary = valid_summary(3U);
    raw_test_file_context_t context = { .file = file };
    seismic_frame_t staging[2];
    seismic_frame_t frames[2];
    size_t frame_count_out = 0U;

    /* start_frame beyond the session. */
    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        4U, 0U, staging, 2U, frames, 2U, &frame_count_out
    ) == ESP_ERR_INVALID_ARG);

    /* frame_count runs past the end of the session. */
    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary,
        2U, 2U, staging, 2U, frames, 2U, &frame_count_out
    ) == ESP_ERR_INVALID_ARG);

    /* Invalid arguments: NULL/zero-capacity. */
    assert(seismic_recorder_waveform_read_raw_source(
        NULL, &context, file_size, &summary, 0U, 0U, staging, 2U, frames, 2U, &frame_count_out
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary, 0U, 0U, staging, 0U, frames, 2U, &frame_count_out
    ) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_waveform_read_raw_source(
        raw_test_read_at, &context, file_size, &summary, 0U, 0U, staging, 2U, frames, 2U, NULL
    ) == ESP_ERR_INVALID_ARG);

    assert(fclose(file) == 0);
}


int main(void)
{
    test_full_session_preserves_exact_samples();
    test_partial_range_and_staging_smaller_than_range();
    test_capacity_too_small_fails_closed_without_truncating();
    test_non_monotonic_timestamps_are_rejected();
    test_non_finite_sample_is_rejected();
    test_range_argument_validation();
    puts("seismic recorder raw waveform tests passed");
    return 0;
}
