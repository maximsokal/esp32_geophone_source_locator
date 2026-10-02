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
    /* The cache is exactly full here; the next dirty frame must report bounded pressure. */
    assert(cache.slots == storage);
    assert(seismic_recorder_cache_append(&cache, 5U, &frames[4], 1U) == ESP_ERR_NO_MEM);

    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.resident_frames == 4U);
    assert(info.dirty_frames == 4U);
    assert(info.persisted_resident_frames == 0U);
    seismic_frame_t preserved[4] = {0};
    uint64_t preserved_sources[4] = {0};
    size_t preserved_count = 0U;
    uint64_t preserved_first = UINT64_MAX;
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache,
        preserved,
        preserved_sources,
        4U,
        &preserved_count,
        &preserved_first
    ) == ESP_OK);
    assert(preserved_first == 0U && preserved_count == 4U);
    for (size_t index = 0U; index < preserved_count; ++index) {
        assert(preserved_sources[index] == index + 1U);
        assert(preserved[index].timestamp_us == frames[index].timestamp_us);
    }

    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(
        &cache, 0U, 2U, &last_source
    ) == ESP_OK);
    assert(last_source == 2U);
    /* The recorder may retry this same ring copy after the persisted prefix frees capacity. */
    assert(seismic_recorder_cache_append(&cache, 5U, &frames[4], 1U) == ESP_OK);
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache,
        preserved,
        preserved_sources,
        4U,
        &preserved_count,
        &preserved_first
    ) == ESP_OK);
    assert(preserved_first == 2U && preserved_count == 3U);
    assert(preserved_sources[0] == 3U && preserved_sources[2] == 5U);
    assert(preserved[0].timestamp_us == frames[2].timestamp_us);
    assert(preserved[2].timestamp_us == frames[4].timestamp_us);
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


static void test_repeated_fill_persist_wrap_cycles(void)
{
    seismic_recorder_cache_slot_t storage[7];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 7U) == ESP_OK);

    seismic_frame_t frames[7] = {0};
    seismic_frame_t dirty[7] = {0};
    uint64_t dirty_sources[7] = {0};
    size_t dirty_count = 0U;
    uint64_t dirty_first = UINT64_MAX;
    uint64_t last_source = 0U;
    for (uint64_t cycle = 0U; cycle < 9U; ++cycle) {
        const uint64_t source_base = 1000U + cycle * 100U;
        fill_frames(frames, source_base, 5U);
        assert(seismic_recorder_cache_append(&cache, source_base, frames, 5U) == ESP_OK);
        assert(seismic_recorder_cache_copy_dirty_with_source_indices(
            &cache, dirty, dirty_sources, 7U, &dirty_count, &dirty_first
        ) == ESP_OK);
        assert(dirty_count == 5U && dirty_first == cycle * 12U);
        assert(seismic_recorder_cache_mark_persisted(
            &cache, dirty_first, dirty_count, &last_source
        ) == ESP_OK);
        assert(last_source == source_base + 4U);

        fill_frames(frames, source_base + 10U, 4U);
        assert(seismic_recorder_cache_append(&cache, source_base + 10U, frames, 4U) == ESP_OK);
        assert(seismic_recorder_cache_copy_dirty_with_source_indices(
            &cache, dirty, dirty_sources, 7U, &dirty_count, &dirty_first
        ) == ESP_OK);
        assert(dirty_count == 4U && dirty_first == cycle * 12U + 5U);
        assert(dirty_sources[0] == source_base + 10U);
        assert(seismic_recorder_cache_mark_persisted(
            &cache, dirty_first, dirty_count, &last_source
        ) == ESP_OK);
        assert(last_source == source_base + 13U);

        fill_frames(frames, source_base + 20U, 3U);
        assert(seismic_recorder_cache_append(&cache, source_base + 20U, frames, 3U) == ESP_OK);
        assert(seismic_recorder_cache_copy_dirty_with_source_indices(
            &cache, dirty, dirty_sources, 7U, &dirty_count, &dirty_first
        ) == ESP_OK);
        assert(dirty_count == 3U && dirty_first == cycle * 12U + 9U);
        assert(dirty_sources[0] == source_base + 20U);
        assert(seismic_recorder_cache_mark_persisted(
            &cache, dirty_first, dirty_count, &last_source
        ) == ESP_OK);
        assert(last_source == source_base + 22U);

        seismic_recorder_cache_info_t info;
        assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
        assert(info.capacity_frames == 7U);
        assert(info.resident_frames == 7U);
        assert(info.dirty_frames == 0U);
        assert(info.next_session_frame == (cycle + 1U) * 12U);
        assert(info.persisted_session_frame == info.next_session_frame);
        assert(cache.slots == storage);
    }
}


static void test_source_gaps_survive_cache_slot_wrap(void)
{
    seismic_recorder_cache_slot_t storage[4];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 4U) == ESP_OK);

    seismic_frame_t first[4] = {0};
    fill_frames(first, 100U, 4U);
    first[2].timestamp_us = 220000LL;
    first[3].timestamp_us = 222000LL;
    assert(seismic_recorder_cache_append_with_source_indices(
        &cache,
        100U,
        first,
        (const uint64_t[]){100U, 101U, 110U, 111U},
        4U
    ) == ESP_OK);
    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(&cache, 0U, 3U, &last_source) == ESP_OK);
    assert(last_source == 102U);

    seismic_frame_t wrapped[2] = {0};
    fill_frames(wrapped, 150U, 2U);
    assert(seismic_recorder_cache_append_with_source_indices(
        &cache,
        150U,
        wrapped,
        (const uint64_t[]){150U, 151U},
        2U
    ) == ESP_OK);

    seismic_frame_t dirty[3] = {0};
    uint64_t dirty_sources[3] = {0};
    size_t dirty_count = 0U;
    uint64_t dirty_first = UINT64_MAX;
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache, dirty, dirty_sources, 3U, &dirty_count, &dirty_first
    ) == ESP_OK);
    /* Slot 3 then slots 0/1 prove the dirty tail and source gaps survive ring wrap. */
    assert(dirty_first == 3U && dirty_count == 3U);
    assert(dirty_sources[0] == 111U);
    assert(dirty_sources[1] == 150U && dirty_sources[2] == 151U);
    assert(dirty[0].timestamp_us == 222000LL);
    assert(dirty[1].timestamp_us == 300000LL);
    assert(seismic_recorder_cache_mark_persisted(
        &cache, dirty_first, dirty_count, &last_source
    ) == ESP_OK);
    assert(last_source == 151U);
}


static void test_stop_drains_partially_dirty_cache_tail(void)
{
    seismic_recorder_cache_slot_t storage[16];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, storage, 16U) == ESP_OK);

    seismic_frame_t frames[9] = {0};
    fill_frames(frames, 200U, 9U);
    assert(seismic_recorder_cache_append(&cache, 200U, frames, 9U) == ESP_OK);
    uint64_t last_source = 0U;
    assert(seismic_recorder_cache_mark_persisted(&cache, 0U, 6U, &last_source) == ESP_OK);
    assert(last_source == 205U);

    seismic_frame_t stop_tail[3] = {0};
    uint64_t stop_sources[3] = {0};
    size_t stop_count = 0U;
    uint64_t stop_first = UINT64_MAX;
    assert(seismic_recorder_cache_copy_dirty_with_source_indices(
        &cache, stop_tail, stop_sources, 16U, &stop_count, &stop_first
    ) == ESP_OK);
    /* STOP drains its final 3-frame dirty tail even though it is below normal batch size. */
    assert(stop_first == 6U && stop_count == 3U);
    assert(stop_sources[0] == 206U && stop_sources[2] == 208U);
    assert(stop_tail[0].timestamp_us == frames[6].timestamp_us);
    assert(seismic_recorder_cache_mark_persisted(
        &cache, stop_first, stop_count, &last_source
    ) == ESP_OK);
    assert(last_source == 208U);

    seismic_recorder_cache_info_t info;
    assert(seismic_recorder_cache_get_info(&cache, &info) == ESP_OK);
    assert(info.dirty_frames == 0U && info.persisted_session_frame == 9U);
}


int main(void)
{
    test_dirty_frames_are_never_evicted();
    test_persisted_history_evicts_before_dirty_tail();
    test_persisted_marking_must_be_in_order();
    test_dense_session_frames_preserve_source_gaps();
    test_repeated_fill_persist_wrap_cycles();
    test_source_gaps_survive_cache_slot_wrap();
    test_stop_drains_partially_dirty_cache_tail();
    puts("seismic recorder cache tests passed");
    return 0;
}
