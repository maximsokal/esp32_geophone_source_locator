#include "seismic_recorder_lod.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>


static seismic_frame_t make_frame(uint64_t index)
{
    seismic_frame_t frame = {
        .timestamp_us = (int64_t)index * 2000LL,
        .channel_uv = {
            (float)index,
            -(float)index,
            100.0f + (float)index,
            -100.0f - (float)index,
        },
    };
    return frame;
}


static void fill_frames(
    seismic_frame_t *frames,
    uint64_t first,
    size_t count
)
{
    for (size_t index = 0U; index < count; ++index) {
        frames[index] = make_frame(first + (uint64_t)index);
    }
}


static void test_disk_contract_is_stable(void)
{
    assert(sizeof(seismic_recorder_lod_file_header_t) == 32U);
    assert(sizeof(seismic_recorder_lod_record_t) == 64U);

    seismic_recorder_lod_file_header_t header;
    seismic_recorder_lod_header_init(&header, 500U);
    assert(seismic_recorder_lod_header_valid(&header, 500U));
    assert(!seismic_recorder_lod_header_valid(&header, 1000U));

    header.reserved = 1U;
    assert(!seismic_recorder_lod_header_valid(&header, 500U));

    uint64_t file_size = 0U;
    assert(seismic_recorder_lod_expected_file_size(0U, &file_size));
    assert(file_size == 32U);
    assert(seismic_recorder_lod_expected_file_size(63U, &file_size));
    assert(file_size == 32U);
    assert(seismic_recorder_lod_expected_file_size(64U, &file_size));
    assert(file_size == 96U);
    assert(seismic_recorder_lod_expected_file_size(128U, &file_size));
    assert(file_size == 160U);
}


static void test_spikes_survive_minmax_decimation(void)
{
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);

    seismic_frame_t frames[SEISMIC_RECORDER_LOD_BLOCK_FRAMES];
    fill_frames(frames, 0U, SEISMIC_RECORDER_LOD_BLOCK_FRAMES);
    frames[17].channel_uv[0] = 12345.0f;
    frames[42].channel_uv[1] = -23456.0f;
    frames[9].channel_uv[2] = -777.0f;
    frames[60].channel_uv[3] = 888.0f;

    seismic_recorder_lod_record_t record;
    size_t record_count = 0U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        frames,
        SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        &record,
        1U,
        &record_count
    ) == ESP_OK);

    assert(record_count == 1U);
    assert(record.first_session_frame == 0U);
    assert(record.frame_count == SEISMIC_RECORDER_LOD_BLOCK_FRAMES);
    assert(record.start_timestamp_us == 0LL);
    assert(record.end_timestamp_us == 126000LL);
    assert(record.max_uv[0] == 12345.0f);
    assert(record.min_uv[1] == -23456.0f);
    assert(record.min_uv[2] == -777.0f);
    assert(record.max_uv[3] == 888.0f);
    assert(builder.next_session_frame == 64U);
    assert(builder.pending_count == 0U);
}


static void test_chunk_boundaries_keep_dense_session_order(void)
{
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);

    seismic_frame_t first[17];
    seismic_frame_t second[111];
    fill_frames(first, 100U, 17U);
    fill_frames(second, 117U, 111U);

    seismic_recorder_lod_record_t records[2];
    size_t record_count = 99U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        first,
        17U,
        records,
        2U,
        &record_count
    ) == ESP_OK);
    assert(record_count == 0U);
    assert(builder.pending_count == 17U);

    assert(seismic_recorder_lod_builder_append(
        &builder,
        17U,
        second,
        111U,
        records,
        2U,
        &record_count
    ) == ESP_OK);
    assert(record_count == 2U);
    assert(records[0].first_session_frame == 0U);
    assert(records[1].first_session_frame == 64U);
    assert(records[0].start_timestamp_us == 200000LL);
    assert(records[0].end_timestamp_us == 326000LL);
    assert(records[1].start_timestamp_us == 328000LL);
    assert(records[1].end_timestamp_us == 454000LL);
    assert(builder.next_session_frame == 128U);
    assert(builder.pending_count == 0U);
}


static void test_session_gap_rejection_is_transactional(void)
{
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 10U);

    seismic_frame_t frames[8];
    fill_frames(frames, 10U, 8U);
    seismic_recorder_lod_record_t records[1];
    memset(records, 0x5a, sizeof(records));

    const seismic_recorder_lod_builder_t before = builder;
    size_t record_count = 7U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        11U,
        frames,
        8U,
        records,
        1U,
        &record_count
    ) == ESP_ERR_INVALID_STATE);
    assert(record_count == 0U);
    assert(memcmp(&builder, &before, sizeof(builder)) == 0);
}


static void test_invalid_samples_and_capacity_do_not_mutate_builder(void)
{
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);

    seismic_frame_t frames[64];
    fill_frames(frames, 0U, 64U);
    seismic_recorder_lod_record_t record;
    size_t record_count = 0U;

    const seismic_recorder_lod_builder_t empty = builder;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        frames,
        64U,
        &record,
        0U,
        &record_count
    ) == ESP_ERR_INVALID_SIZE);
    assert(memcmp(&builder, &empty, sizeof(builder)) == 0);

    frames[23].channel_uv[2] = NAN;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        frames,
        64U,
        &record,
        1U,
        &record_count
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(memcmp(&builder, &empty, sizeof(builder)) == 0);
}


static void test_timestamp_regression_is_rejected_before_mutation(void)
{
    seismic_recorder_lod_builder_t builder;
    seismic_recorder_lod_builder_reset(&builder, 0U);

    seismic_frame_t prefix[10];
    fill_frames(prefix, 0U, 10U);
    seismic_recorder_lod_record_t records[1];
    size_t record_count = 0U;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        0U,
        prefix,
        10U,
        records,
        1U,
        &record_count
    ) == ESP_OK);

    seismic_frame_t next[2];
    fill_frames(next, 10U, 2U);
    next[0].timestamp_us = prefix[9].timestamp_us - 1LL;

    const seismic_recorder_lod_builder_t before = builder;
    assert(seismic_recorder_lod_builder_append(
        &builder,
        10U,
        next,
        2U,
        records,
        1U,
        &record_count
    ) == ESP_ERR_INVALID_RESPONSE);
    assert(memcmp(&builder, &before, sizeof(builder)) == 0);
}


int main(void)
{
    test_disk_contract_is_stable();
    test_spikes_survive_minmax_decimation();
    test_chunk_boundaries_keep_dense_session_order();
    test_session_gap_rejection_is_transactional();
    test_invalid_samples_and_capacity_do_not_mutate_builder();
    test_timestamp_regression_is_rejected_before_mutation();
    puts("seismic recorder LOD tests passed");
    return 0;
}
