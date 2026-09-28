#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seismic_buffer.h"


typedef struct
{
    uint64_t source_frame_number;
    uint64_t source_sample_index;
    seismic_frame_t frame;

} seismic_recorder_cache_slot_t;


typedef struct
{
    seismic_recorder_cache_slot_t *slots;
    size_t capacity;
    size_t head;
    size_t count;
    uint64_t first_session_frame;
    uint64_t next_session_frame;
    uint64_t persisted_session_frame;
    bool initialized;

} seismic_recorder_cache_t;


typedef struct
{
    size_t capacity_frames;
    size_t resident_frames;
    size_t dirty_frames;
    size_t persisted_resident_frames;
    uint64_t first_session_frame;
    uint64_t next_session_frame;
    uint64_t persisted_session_frame;

} seismic_recorder_cache_info_t;


esp_err_t seismic_recorder_cache_init(
    seismic_recorder_cache_t *cache,
    seismic_recorder_cache_slot_t *storage,
    size_t capacity
);


void seismic_recorder_cache_reset(
    seismic_recorder_cache_t *cache
);


esp_err_t seismic_recorder_cache_append(
    seismic_recorder_cache_t *cache,
    uint64_t first_source_frame_number,
    const seismic_frame_t *frames,
    size_t frame_count
);


esp_err_t seismic_recorder_cache_append_with_source_indices(
    seismic_recorder_cache_t *cache,
    uint64_t first_source_frame_number,
    const seismic_frame_t *frames,
    const uint64_t *source_sample_indices,
    size_t frame_count
);


esp_err_t seismic_recorder_cache_copy_dirty(
    const seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *first_session_frame
);


esp_err_t seismic_recorder_cache_copy_dirty_with_source_indices(
    const seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *source_frame_numbers,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *first_session_frame
);


esp_err_t seismic_recorder_cache_mark_persisted(
    seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    size_t frame_count,
    uint64_t *last_source_frame_number
);


esp_err_t seismic_recorder_cache_copy_range(
    const seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
);


bool seismic_recorder_cache_contains_range(
    const seismic_recorder_cache_t *cache,
    uint64_t first_session_frame,
    uint64_t frame_count
);


esp_err_t seismic_recorder_cache_get_info(
    const seismic_recorder_cache_t *cache,
    seismic_recorder_cache_info_t *info
);
