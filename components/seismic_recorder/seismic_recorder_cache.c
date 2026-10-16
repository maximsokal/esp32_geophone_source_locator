#include "seismic_recorder_cache.h"

#include <limits.h>
#include <string.h>


static size_t seismic_recorder_cache_oldest_index(
    const seismic_recorder_cache_t *cache
)
{
    if (cache->count == 0U) {
        return cache->head;
    }

    return (
        cache->head +
        cache->capacity -
        cache->count
    ) % cache->capacity;
}


static bool seismic_recorder_cache_add_overflows_u64(
    uint64_t left,
    uint64_t right,
    uint64_t *result
)
{
    if (right > UINT64_MAX - left) {
        return true;
    }
    *result = left + right;
    return false;
}


static uint64_t seismic_recorder_cache_saturating_add_u64(
    uint64_t value,
    uint64_t amount
)
{
    return amount > UINT64_MAX - value ? UINT64_MAX : value + amount;
}


static const seismic_recorder_cache_slot_t *seismic_recorder_cache_slot_for_session_frame(
    const seismic_recorder_cache_t *cache,
    uint64_t session_frame
)
{
    if (
        cache == NULL ||
        !cache->initialized ||
        cache->count == 0U ||
        session_frame < cache->first_session_frame ||
        session_frame >= cache->next_session_frame
    ) {
        return NULL;
    }

    const uint64_t offset64 =
        session_frame - cache->first_session_frame;
    if (offset64 >= (uint64_t)cache->count) {
        return NULL;
    }

    const size_t oldest = seismic_recorder_cache_oldest_index(cache);
    const size_t index =
        (oldest + (size_t)offset64) % cache->capacity;
    return &cache->slots[index];
}


esp_err_t seismic_recorder_cache_init(
    seismic_recorder_cache_t *cache,
    seismic_recorder_cache_slot_t *storage,
    size_t capacity
)
{
    if (cache == NULL || storage == NULL || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cache, 0, sizeof(*cache));
    cache->slots = storage;
    cache->capacity = capacity;
    cache->initialized = true;
    return ESP_OK;
}


void seismic_recorder_cache_reset(
    seismic_recorder_cache_t *cache
)
{
    if (cache == NULL || !cache->initialized) {
        return;
    }

    cache->head = 0U;
    cache->count = 0U;
    cache->first_session_frame = 0U;
    cache->next_session_frame = 0U;
    cache->persisted_session_frame = 0U;
    cache->last_source_sample_index = 0U;
    cache->source_gap_samples = 0U;
    cache->has_source_sample_index = false;
}


esp_err_t seismic_recorder_cache_append_with_source_indices(
    seismic_recorder_cache_t *cache,
    uint64_t first_source_frame_number,
    const seismic_frame_t *frames,
    const uint64_t *source_sample_indices,
    size_t frame_count,
    uint64_t ring_dropped_frames_before
)
{
    if (
        cache == NULL ||
        !cache->initialized ||
        frames == NULL ||
        frame_count == 0U ||
        first_source_frame_number == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t last_source_frame = 0U;
    if (seismic_recorder_cache_add_overflows_u64(
            first_source_frame_number,
            (uint64_t)frame_count - 1U,
            &last_source_frame
        )) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)last_source_frame;

    const uint64_t dirty64 =
        cache->next_session_frame -
        cache->persisted_session_frame;
    if (dirty64 > (uint64_t)cache->capacity) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t dirty = (size_t)dirty64;
    if (frame_count > cache->capacity - dirty) {
        /* Unpersisted samples are never overwritten. The caller must drain
         * them to SD before trying to append more acquisition data. */
        return ESP_ERR_NO_MEM;
    }

    const size_t required_evict =
        cache->count + frame_count > cache->capacity
            ? cache->count + frame_count - cache->capacity
            : 0U;

    const uint64_t persisted_resident64 =
        cache->persisted_session_frame > cache->first_session_frame
            ? cache->persisted_session_frame - cache->first_session_frame
            : 0U;
    const size_t persisted_resident =
        persisted_resident64 > (uint64_t)cache->count
            ? cache->count
            : (size_t)persisted_resident64;

    if (required_evict > persisted_resident) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t source_gap_samples = cache->source_gap_samples;
    uint64_t previous_sample_index = cache->last_source_sample_index;
    uint64_t unaccounted_ring_drops = ring_dropped_frames_before;
    bool has_previous_sample_index = cache->has_source_sample_index;
    for (size_t index = 0U; index < frame_count; ++index) {
        const uint64_t sample_index = source_sample_indices != NULL
            ? source_sample_indices[index]
            : first_source_frame_number + (uint64_t)index;
        if (has_previous_sample_index) {
            if (sample_index <= previous_sample_index) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            const uint64_t missing = sample_index - previous_sample_index - 1U;
            const uint64_t ring_explained = index == 0U
                ? (missing < unaccounted_ring_drops
                    ? missing
                    : unaccounted_ring_drops)
                : 0U;
            unaccounted_ring_drops -= ring_explained;
            source_gap_samples = seismic_recorder_cache_saturating_add_u64(
                source_gap_samples,
                missing - ring_explained
            );
        }
        previous_sample_index = sample_index;
        has_previous_sample_index = true;
    }

    cache->first_session_frame += (uint64_t)required_evict;
    cache->count -= required_evict;

    for (size_t index = 0U; index < frame_count; ++index) {
        seismic_recorder_cache_slot_t *slot =
            &cache->slots[cache->head];
        slot->source_frame_number =
            first_source_frame_number + (uint64_t)index;
        slot->source_sample_index = source_sample_indices != NULL
            ? source_sample_indices[index]
            : first_source_frame_number + (uint64_t)index;
        slot->frame = frames[index];

        cache->head++;
        if (cache->head >= cache->capacity) {
            cache->head = 0U;
        }
        cache->count++;
        cache->next_session_frame++;
    }

    cache->last_source_sample_index = previous_sample_index;
    cache->has_source_sample_index = has_previous_sample_index;
    cache->source_gap_samples = source_gap_samples;

    return ESP_OK;
}


bool seismic_recorder_cache_contains_range(
    const seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    uint64_t frame_count
)
{
    if (cache == NULL || !cache->initialized) {
        return false;
    }

    if (frame_count == 0U) {
        return first_session_frame >= cache->first_session_frame &&
               first_session_frame <= cache->next_session_frame;
    }

    uint64_t end_exclusive = 0U;
    if (seismic_recorder_cache_add_overflows_u64(
            first_session_frame,
            frame_count,
            &end_exclusive
        )) {
        return false;
    }

    return
        first_session_frame >= cache->first_session_frame &&
        end_exclusive <= cache->next_session_frame;
}


esp_err_t seismic_recorder_cache_copy_range(
    const seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
)
{
    if (
        cache == NULL ||
        !cache->initialized ||
        frames == NULL ||
        frame_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!seismic_recorder_cache_contains_range(
            cache,
            first_session_frame,
            (uint64_t)frame_count
        )) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t oldest = seismic_recorder_cache_oldest_index(cache);
    const size_t offset =
        (size_t)(first_session_frame - cache->first_session_frame);
    size_t source = (oldest + offset) % cache->capacity;

    size_t copied = 0U;
    while (copied < frame_count) {
        const size_t until_wrap = cache->capacity - source;
        const size_t remaining = frame_count - copied;
        const size_t chunk = remaining < until_wrap
            ? remaining
            : until_wrap;

        for (size_t index = 0U; index < chunk; ++index) {
            frames[copied + index] = cache->slots[source + index].frame;
        }

        copied += chunk;
        source = 0U;
    }

    return ESP_OK;
}


esp_err_t seismic_recorder_cache_copy_dirty_with_source_indices(
    const seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *source_frame_numbers,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *first_session_frame
)
{
    if (
        cache == NULL ||
        !cache->initialized ||
        frames == NULL ||
        max_frames == 0U ||
        frame_count == NULL ||
        first_session_frame == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frame_count = 0U;
    *first_session_frame = cache->persisted_session_frame;

    const uint64_t dirty64 =
        cache->next_session_frame -
        cache->persisted_session_frame;
    if (dirty64 == 0U) {
        return ESP_OK;
    }

    const size_t dirty = dirty64 > (uint64_t)SIZE_MAX
        ? SIZE_MAX
        : (size_t)dirty64;
    const size_t count = dirty < max_frames
        ? dirty
        : max_frames;

    const size_t oldest = seismic_recorder_cache_oldest_index(cache);
    const size_t offset = (size_t)(
        cache->persisted_session_frame - cache->first_session_frame
    );
    size_t source = (oldest + offset) % cache->capacity;
    for (size_t index = 0U; index < count; ++index) {
        const seismic_recorder_cache_slot_t *slot = &cache->slots[source];
        frames[index] = slot->frame;
        if (source_frame_numbers != NULL) {
            source_frame_numbers[index] = slot->source_sample_index;
        }
        if (++source == cache->capacity) {
            source = 0U;
        }
    }

    *frame_count = count;
    return ESP_OK;
}


esp_err_t seismic_recorder_cache_append(
    seismic_recorder_cache_t *cache,
    uint64_t first_source_frame_number,
    const seismic_frame_t *frames,
    size_t frame_count
)
{
    return seismic_recorder_cache_append_with_source_indices(
        cache,
        first_source_frame_number,
        frames,
        NULL,
        frame_count,
        0U
    );
}


esp_err_t seismic_recorder_cache_copy_dirty(
    const seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *first_session_frame
)
{
    return seismic_recorder_cache_copy_dirty_with_source_indices(
        cache,
        frames,
        NULL,
        max_frames,
        frame_count,
        first_session_frame
    );
}


esp_err_t seismic_recorder_cache_mark_persisted(
    seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    size_t frame_count,
    uint64_t *last_source_frame_number
)
{
    if (
        cache == NULL ||
        !cache->initialized ||
        frame_count == 0U ||
        last_source_frame_number == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (first_session_frame != cache->persisted_session_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t end_exclusive = 0U;
    if (seismic_recorder_cache_add_overflows_u64(
            first_session_frame,
            (uint64_t)frame_count,
            &end_exclusive
        ) ||
        end_exclusive > cache->next_session_frame
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    const seismic_recorder_cache_slot_t *last =
        seismic_recorder_cache_slot_for_session_frame(
            cache,
            end_exclusive - 1U
        );
    if (last == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cache->persisted_session_frame = end_exclusive;
    *last_source_frame_number = last->source_frame_number;
    return ESP_OK;
}


esp_err_t seismic_recorder_cache_get_info(
    const seismic_recorder_cache_t *cache,
    seismic_recorder_cache_info_t *info
)
{
    if (cache == NULL || !cache->initialized || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t dirty64 =
        cache->next_session_frame -
        cache->persisted_session_frame;
    const uint64_t persisted_resident64 =
        cache->persisted_session_frame > cache->first_session_frame
            ? cache->persisted_session_frame - cache->first_session_frame
            : 0U;

    info->capacity_frames = cache->capacity;
    info->resident_frames = cache->count;
    info->dirty_frames = dirty64 > (uint64_t)SIZE_MAX
        ? SIZE_MAX
        : (size_t)dirty64;
    info->persisted_resident_frames =
        persisted_resident64 > (uint64_t)cache->count
            ? cache->count
            : (size_t)persisted_resident64;
    info->first_session_frame = cache->first_session_frame;
    info->next_session_frame = cache->next_session_frame;
    info->persisted_session_frame = cache->persisted_session_frame;
    info->source_gap_samples = cache->source_gap_samples;
    return ESP_OK;
}
