#include "seismic_recorder_cache.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>


static seismic_frame_t make_frame(uint64_t source_frame)
{
    seismic_frame_t frame = {
        .timestamp_us = (int64_t)source_frame * 2000LL,
        .channel_uv = {
            (float)source_frame,
            -(float)source_frame,
            (float)source_frame + 0.25f,
            (float)source_frame - 0.25f,
        },
    };
    return frame;
}


static void fill_frames(
    seismic_frame_t *frames,
    uint64_t first_source_frame,
    size_t count
)
{
    for (size_t index = 0U; index < count; ++index) {
        frames[index] = make_frame(first_source_frame + (uint64_t)index);
    }
}


static void test_dirty_frames_are_never_evicted(void)
{
    seismic_recorder_cache_slot_t storage[4];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 4U) == ESP_OK);

    seismic_frame_t frames[5];
    fill_frames(frames, 1U, 5U);

    assert(seismic_recorder_cache_append(&cache, 1U, frames, 4U) == ESP_OK);
    assert(seismic_recorder_cache_append(&cache, 5U, &frames[4], 1U) == ESP_ERR_NO_MEM);

    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.resident_frames == 4U);
    assert(info.dirty_frames == 4U);
    assert(info.persisted_resident_frames == 0U);
}


static void test_persisted_history_evicts_before_dirty_tail(void)
{
    seismic_recorder_cache_slot_t storage[4];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 4U) == ESP_OK);

    seismic_frame_t first[3];
    fill_frames(first, 10U, 3U);
    assert(seismic_recorder_cache_append(&cache, 10U, first, 3U) == ESP_OK);

    seismic_frame_t dirty[4] = {0};
    uint64_t dirty_sources[4] = {0};
    size_t dirty_count = 0U;
    uint64_t dirty_first = UINT64_MAX;
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache,
        dirty,
        dirty_sources,
        2U,
        &dirty_count,
        &dirty_first
    ) == ESP_OK);
    assert(dirty_first == 0U && dirty_count == 2U);
    assert(dirty_sources[0] == 10U && dirty_sources[1] == 11U);
    assert(dirty[0].timestamp_us == 20000LL);
    assert(dirty[1].timestamp_us == 22000LL);

    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(
        &cache,
        0U,
        2U,
        &last_source
    ) == ESP_OK);
    assert(last_source == 11U);

    seismic_frame_t second[2];
    fill_frames(second, 13U, 2U);
    assert(seismic_recorder_cache_append(&cache, 13U, second, 2U) == ESP_OK);

    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.capacity_frames == 4U);
    assert(info.resident_frames == 4U);
    assert(info.first_session_frame == 1U);
    assert(info.next_session_frame == 5U);
    assert(info.persisted_session_frame == 2U);
    assert(info.persisted_resident_frames == 1U);
    assert(info.dirty_frames == 3U);

    assert(!seismic_recorder_cache_contains_range(&cache, 0U, 1U));
    assert(seismic_recorder_cache_contains_range(&cache, 1U, 4U));

    seismic_frame_t resident[4] = {0};
    assert(seismic_recorder_cache_copy_range(
        &cache,
        1U,
        resident,
        4U
    ) == ESP_OK);
    assert(resident[0].timestamp_us == 22000LL);
    assert(resident[1].timestamp_us == 24000LL);
    assert(resident[2].timestamp_us == 26000LL);
    assert(resident[3].timestamp_us == 28000LL);

    memset(dirty, 0, sizeof(dirty));
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache,
        dirty,
        dirty_sources,
        4U,
        &dirty_count,
        &dirty_first
    ) == ESP_OK);
    assert(dirty_first == 2U && dirty_count == 3U);
    assert(dirty_sources[0] == 12U && dirty_sources[2] == 14U);
    assert(dirty[0].timestamp_us == 24000LL);
    assert(dirty[2].timestamp_us == 28000LL);

    assert(seismic_recorder_cache_mark_persisted(
        &cache,
        dirty_first,
        dirty_count,
        &last_source
    ) == ESP_OK);
    assert(last_source == 14U);

    seismic_frame_t replacement[4];
    fill_frames(replacement, 20U, 4U);
    assert(seismic_recorder_cache_append(&cache, 20U, replacement, 4U) == ESP_OK);
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.first_session_frame == 5U);
    assert(info.next_session_frame == 9U);
    assert(info.persisted_session_frame == 5U);
    assert(info.dirty_frames == 4U);
}


static void test_persisted_marking_must_be_in_order(void)
{
    seismic_recorder_cache_slot_t storage[4];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 4U) == ESP_OK);

    seismic_frame_t frames[2];
    fill_frames(frames, 30U, 2U);
    assert(seismic_recorder_cache_append(&cache, 30U, frames, 2U) == ESP_OK);

    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(
        &cache,
        1U,
        1U,
        &last_source
    ) == ESP_ERR_INVALID_STATE);

    assert(seismic_recorder_cache_mark_persisted(
        &cache,
        0U,
        3U,
        &last_source
    ) == ESP_ERR_INVALID_SIZE);
}


static void test_dense_session_frames_preserve_source_gaps(void)
{
    seismic_recorder_cache_slot_t storage[6];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 6U) == ESP_OK);

    seismic_frame_t before_gap[2];
    seismic_frame_t after_gap[2];
    fill_frames(before_gap, 100U, 2U);
    fill_frames(after_gap, 120U, 2U);

    assert(seismic_recorder_cache_append(&cache, 100U, before_gap, 2U) == ESP_OK);
    assert(seismic_recorder_cache_append(&cache, 120U, after_gap, 2U) == ESP_OK);

    seismic_frame_t dirty[4] = {0};
    uint64_t dirty_sources[4] = {0};
    size_t dirty_count = 0U;
    uint64_t dirty_first = UINT64_MAX;
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache,
        dirty,
        dirty_sources,
        4U,
        &dirty_count,
        &dirty_first
    ) == ESP_OK);
    assert(dirty_first == 0U);
    assert(dirty_count == 4U);
    assert(dirty[0].timestamp_us == 200000LL);
    assert(dirty[1].timestamp_us == 202000LL);
    assert(dirty[2].timestamp_us == 240000LL);
    assert(dirty[3].timestamp_us == 242000LL);
    assert(dirty_sources[0] == 100U && dirty_sources[1] == 101U);
    assert(dirty_sources[2] == 120U && dirty_sources[3] == 121U);

    /* Session ordinals stay dense while retained source indexes preserve the gap. */
    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(
        &cache,
        0U,
        4U,
        &last_source
    ) == ESP_OK);
    assert(last_source == 121U);

    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.next_session_frame == 4U);
    assert(info.persisted_session_frame == 4U);
    assert(info.dirty_frames == 0U);
    assert(info.persisted_resident_frames == 4U);
}


int main(void)
{
    test_dirty_frames_are_never_evicted();
    test_persisted_history_evicts_before_dirty_tail();
    test_persisted_marking_must_be_in_order();
    test_dense_session_frames_preserve_source_gaps();
    puts("seismic recorder cache tests passed");
    return 0;
}
