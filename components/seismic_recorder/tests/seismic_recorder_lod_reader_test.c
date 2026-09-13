#include "seismic_recorder_lod_reader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct
{
    const unsigned char *data;
    size_t size;
    size_t bytes_read;
    size_t calls;

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
        reader == NULL ||
        buffer == NULL ||
        size_bytes == 0U ||
        offset_bytes > reader->size ||
        size_bytes > reader->size - (size_t)offset_bytes
    ) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(buffer, reader->data + (size_t)offset_bytes, size_bytes);
    reader->bytes_read += size_bytes;
    reader->calls += 1U;
    return ESP_OK;
}


static seismic_frame_t make_frame(uint64_t index)
{
    seismic_frame_t frame = {
        .timestamp_us = 1000000LL + (int64_t)index * 2000LL,
        .channel_uv = {
            (float)((int64_t)(index % 301U) - 150LL),
            (float)((int64_t)(index % 197U) - 98LL) * 2.0f,
            (float)((int64_t)(index % 113U) - 56LL) * 3.0f,
            (float)((int64_t)(index % 71U) - 35LL) * 4.0f,
        },
    };

    if (index == 777U) frame.channel_uv[0] = 123456.0f;
    if (index == 2049U) frame.channel_uv[1] = -654321.0f;
    if (index == 3090U) frame.channel_uv[2] = 222222.0f;
    if (index == 4001U) frame.channel_uv[3] = -333333.0f;
    return frame;
}


typedef struct
{
    unsigned char *raw_file;
    size_t raw_size;
    unsigned char *lod_file;
    size_t lod_size;
    seismic_frame_t *frames;
    uint64_t frame_count;
    seismic_recorder_session_summary_t summary;

} fixture_t;


static void fixture_destroy(fixture_t *fixture)
{
    free(fixture->raw_file);
    free(fixture->lod_file);
    free(fixture->frames);
    memset(fixture, 0, sizeof(*fixture));
}


static void fixture_init(fixture_t *fixture, uint64_t frame_count)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->frame_count = frame_count;
    fixture->frames = calloc((size_t)frame_count, sizeof(*fixture->frames));
    assert(fixture->frames != NULL);

    for (uint64_t index = 0U; index < frame_count; ++index) {
        fixture->frames[index] = make_frame(index);
    }

    fixture->raw_size =
        sizeof(seismic_recorder_binary_header_t) +
        (size_t)frame_count * sizeof(seismic_frame_t);
    fixture->raw_file = calloc(1U, fixture->raw_size);
    assert(fixture->raw_file != NULL);

    seismic_recorder_binary_header_t raw_header = {
        .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
        .version = SEISMIC_RECORDER_BINARY_VERSION,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .sample_rate_hz = 500U,
        .frame_size_bytes = sizeof(seismic_frame_t),
        .reserved = 0U,
    };
    memcpy(fixture->raw_file, &raw_header, sizeof(raw_header));
    memcpy(
        fixture->raw_file + sizeof(raw_header),
        fixture->frames,
        (size_t)frame_count * sizeof(seismic_frame_t)
    );

    uint64_t lod_size_u64 = 0U;
    assert(seismic_recorder_lod_expected_file_size(frame_count, &lod_size_u64));
    fixture->lod_size = (size_t)lod_size_u64;
    fixture->lod_file = calloc(1U, fixture->lod_size);
    assert(fixture->lod_file != NULL);

    seismic_recorder_lod_file_header_t lod_header;
    seismic_recorder_lod_header_init(&lod_header, 500U);
    seismic_recorder_lod_header_mark_complete(&lod_header);
    memcpy(fixture->lod_file, &lod_header, sizeof(lod_header));

    const size_t record_capacity =
        (size_t)(frame_count / SEISMIC_RECORDER_LOD_BLOCK_FRAMES);
    seismic_recorder_lod_record_t *records =
        (seismic_recorder_lod_record_t *)(
            fixture->lod_file + sizeof(lod_header)
        );

    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);
    size_t record_count = 0U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        fixture->frames,
        (size_t)frame_count,
        records,
        record_capacity,
        &record_count
    ) == ESP_OK);
    assert(record_count == record_capacity);

    memset(&fixture->summary, 0, sizeof(fixture->summary));
    snprintf(
        fixture->summary.session_id,
        sizeof(fixture->summary.session_id),
        "%s",
        "S000001"
    );
    snprintf(
        fixture->summary.source_name,
        sizeof(fixture->summary.source_name),
        "%s",
        "AD7606"
    );
    fixture->summary.complete = true;
    fixture->summary.sample_rate_hz = 500U;
    fixture->summary.channel_count = SEISMIC_CHANNEL_COUNT;
    fixture->summary.samples_written = frame_count;
    fixture->summary.dropped_frames = 17U;
    fixture->summary.recording_ms = frame_count * 2U;
}


static void assert_waveform_equal(
    const seismic_recorder_waveform_point_t *expected,
    const seismic_recorder_waveform_point_t *actual,
    size_t count,
    const seismic_recorder_waveform_info_t *expected_info,
    const seismic_recorder_waveform_info_t *actual_info
)
{
    assert(expected_info->complete == actual_info->complete);
    assert(expected_info->sample_rate_hz == actual_info->sample_rate_hz);
    assert(expected_info->channel_count == actual_info->channel_count);
    assert(expected_info->samples_written == actual_info->samples_written);
    assert(expected_info->dropped_frames == actual_info->dropped_frames);
    assert(expected_info->recording_ms == actual_info->recording_ms);
    assert(expected_info->source_frame_count == actual_info->source_frame_count);
    assert(expected_info->start_frame == actual_info->start_frame);
    assert(expected_info->frame_count == actual_info->frame_count);
    assert(expected_info->point_count == actual_info->point_count);
    assert(expected_info->start_timestamp_us == actual_info->start_timestamp_us);
    assert(expected_info->end_timestamp_us == actual_info->end_timestamp_us);
    assert(strcmp(expected_info->session_id, actual_info->session_id) == 0);
    assert(strcmp(expected_info->source_name, actual_info->source_name) == 0);

    for (size_t point = 0U; point < count; ++point) {
        assert(expected[point].timestamp_us == actual[point].timestamp_us);
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(expected[point].min_uv[channel] == actual[point].min_uv[channel]);
            assert(expected[point].max_uv[channel] == actual[point].max_uv[channel]);
        }
    }
}


static void test_block_policy_rejects_query_and_bucket_boundaries(void)
{
    size_t point = 99U;

    assert(!seismic_recorder_lod_block_maps_to_one_point(
        13U,
        1000U,
        10U,
        0U,
        &point
    ));

    assert(seismic_recorder_lod_block_maps_to_one_point(
        0U,
        4096U,
        8U,
        64U,
        &point
    ));
    assert(point == 0U);

    /* [448,511] and [512,575] sit on opposite sides of the first 512-frame
     * bucket boundary, so each complete block is independently safe. */
    assert(seismic_recorder_lod_block_maps_to_one_point(
        0U,
        4096U,
        8U,
        448U,
        &point
    ));
    assert(point == 0U);
    assert(seismic_recorder_lod_block_maps_to_one_point(
        0U,
        4096U,
        8U,
        512U,
        &point
    ));
    assert(point == 1U);

    /* With 37 output points the mathematical bucket boundary cuts through
     * some 64-frame blocks; those must use raw data. */
    bool found_crossing = false;
    for (uint64_t block = 0U; block < 4096U; block += 64U) {
        if (!seismic_recorder_lod_block_maps_to_one_point(
                0U,
                4096U,
                37U,
                block,
                &point
            )) {
            found_crossing = true;
            break;
        }
    }
    assert(found_crossing);
}


static void test_hybrid_matches_raw_for_unaligned_long_range(void)
{
    fixture_t fixture;
    fixture_init(&fixture, 4096U);

    memory_reader_t raw_reference_reader = {
        .data = fixture.raw_file,
        .size = fixture.raw_size,
    };
    memory_reader_t hybrid_raw_reader = raw_reference_reader;
    memory_reader_t hybrid_lod_reader = {
        .data = fixture.lod_file,
        .size = fixture.lod_size,
    };

    seismic_recorder_waveform_point_t expected[37];
    seismic_recorder_waveform_point_t actual[37];
    seismic_recorder_waveform_info_t expected_info;
    seismic_recorder_waveform_info_t actual_info;
    seismic_frame_t raw_reference_staging[128];
    seismic_frame_t hybrid_raw_staging[128];
    seismic_recorder_lod_record_t hybrid_lod_staging[8];

    assert(seismic_recorder_waveform_read_source(
        memory_read_at,
        &raw_reference_reader,
        fixture.raw_size,
        &fixture.summary,
        13U,
        4000U,
        37U,
        raw_reference_staging,
        128U,
        expected,
        37U,
        &expected_info
    ) == ESP_OK);

    assert(seismic_recorder_lod_read_source(
        memory_read_at,
        &hybrid_raw_reader,
        fixture.raw_size,
        memory_read_at,
        &hybrid_lod_reader,
        fixture.lod_size,
        &fixture.summary,
        13U,
        4000U,
        37U,
        hybrid_raw_staging,
        128U,
        hybrid_lod_staging,
        8U,
        actual,
        37U,
        &actual_info
    ) == ESP_OK);

    assert_waveform_equal(
        expected,
        actual,
        expected_info.point_count,
        &expected_info,
        &actual_info
    );

    assert(hybrid_lod_reader.calls > 1U);

    /* LOD64 can replace only complete blocks that map wholly into one output
     * bucket. For this deliberately unaligned 4000-frame/37-point request,
     * count that eligibility directly and require the raw-byte saving to equal
     * exactly those substituted frames. This is a correctness/performance
     * contract, not an unrealistic fixed compression ratio. */
    size_t eligible_blocks = 0U;
    const uint64_t request_start = 13U;
    const uint64_t request_count = 4000U;
    const uint64_t request_end = request_start + request_count;
    for (
        uint64_t block = 0U;
        block + SEISMIC_RECORDER_LOD_BLOCK_FRAMES <= request_end;
        block += SEISMIC_RECORDER_LOD_BLOCK_FRAMES
    ) {
        size_t point = 0U;
        if (seismic_recorder_lod_block_maps_to_one_point(
                request_start,
                request_count,
                37U,
                block,
                &point
            )) {
            ++eligible_blocks;
        }
    }
    assert(eligible_blocks > 0U);
    const size_t expected_raw_bytes_saved =
        eligible_blocks *
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES *
        sizeof(seismic_frame_t);
    assert(raw_reference_reader.bytes_read > hybrid_raw_reader.bytes_read);
    assert(
        raw_reference_reader.bytes_read - hybrid_raw_reader.bytes_read ==
        expected_raw_bytes_saved
    );

    fixture_destroy(&fixture);
}


static void test_short_range_falls_back_to_raw_blocks_but_stays_exact(void)
{
    fixture_t fixture;
    fixture_init(&fixture, 4096U);

    memory_reader_t reference_reader = {
        .data = fixture.raw_file,
        .size = fixture.raw_size,
    };
    memory_reader_t hybrid_raw_reader = reference_reader;
    memory_reader_t lod_reader = {
        .data = fixture.lod_file,
        .size = fixture.lod_size,
    };

    seismic_recorder_waveform_point_t expected[50];
    seismic_recorder_waveform_point_t actual[50];
    seismic_recorder_waveform_info_t expected_info;
    seismic_recorder_waveform_info_t actual_info;
    seismic_frame_t staging_a[64];
    seismic_frame_t staging_b[64];
    seismic_recorder_lod_record_t lod_staging[4];

    assert(seismic_recorder_waveform_read_source(
        memory_read_at,
        &reference_reader,
        fixture.raw_size,
        &fixture.summary,
        101U,
        50U,
        50U,
        staging_a,
        64U,
        expected,
        50U,
        &expected_info
    ) == ESP_OK);

    assert(seismic_recorder_lod_read_source(
        memory_read_at,
        &hybrid_raw_reader,
        fixture.raw_size,
        memory_read_at,
        &lod_reader,
        fixture.lod_size,
        &fixture.summary,
        101U,
        50U,
        50U,
        staging_b,
        64U,
        lod_staging,
        4U,
        actual,
        50U,
        &actual_info
    ) == ESP_OK);

    assert_waveform_equal(
        expected,
        actual,
        50U,
        &expected_info,
        &actual_info
    );

    /* Header validation still reads the sidecar, but no LOD data record can
     * represent a one-frame output bucket exactly. */
    assert(lod_reader.calls == 1U);

    fixture_destroy(&fixture);
}


static void test_incomplete_or_corrupt_sidecar_is_rejected(void)
{
    fixture_t fixture;
    fixture_init(&fixture, 4096U);

    seismic_recorder_lod_file_header_t *header =
        (seismic_recorder_lod_file_header_t *)fixture.lod_file;
    header->flags = 0U;

    memory_reader_t raw_reader = {
        .data = fixture.raw_file,
        .size = fixture.raw_size,
    };
    memory_reader_t lod_reader = {
        .data = fixture.lod_file,
        .size = fixture.lod_size,
    };
    seismic_frame_t raw_staging[128];
    seismic_recorder_lod_record_t lod_staging[4];
    seismic_recorder_waveform_point_t points[32];
    seismic_recorder_waveform_info_t info;

    assert(seismic_recorder_lod_read_source(
        memory_read_at,
        &raw_reader,
        fixture.raw_size,
        memory_read_at,
        &lod_reader,
        fixture.lod_size,
        &fixture.summary,
        0U,
        4096U,
        32U,
        raw_staging,
        128U,
        lod_staging,
        4U,
        points,
        32U,
        &info
    ) == ESP_ERR_INVALID_RESPONSE);

    seismic_recorder_lod_header_mark_complete(header);
    seismic_recorder_lod_record_t *records =
        (seismic_recorder_lod_record_t *)(
            fixture.lod_file + sizeof(*header)
        );
    records[0].first_session_frame = 64U;

    raw_reader.bytes_read = 0U;
    raw_reader.calls = 0U;
    lod_reader.bytes_read = 0U;
    lod_reader.calls = 0U;
    assert(seismic_recorder_lod_read_source(
        memory_read_at,
        &raw_reader,
        fixture.raw_size,
        memory_read_at,
        &lod_reader,
        fixture.lod_size,
        &fixture.summary,
        0U,
        4096U,
        32U,
        raw_staging,
        128U,
        lod_staging,
        4U,
        points,
        32U,
        &info
    ) == ESP_ERR_INVALID_RESPONSE);

    fixture_destroy(&fixture);
}


int main(void)
{
    test_block_policy_rejects_query_and_bucket_boundaries();
    test_hybrid_matches_raw_for_unaligned_long_range();
    test_short_range_falls_back_to_raw_blocks_but_stays_exact();
    test_incomplete_or_corrupt_sidecar_is_rejected();
    puts("seismic recorder LOD reader tests passed");
    return 0;
}
