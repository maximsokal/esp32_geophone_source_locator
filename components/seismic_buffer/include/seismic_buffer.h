#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define SEISMIC_CHANNEL_COUNT    4
/* Compatibility for legacy tests/clients; use seismic_buffer_capacity(). */
#define SEISMIC_BUFFER_CAPACITY  2048


typedef enum
{
    SEISMIC_SOURCE_NONE = 0,
    SEISMIC_SOURCE_TEST,
    SEISMIC_SOURCE_AD7606 = 3

} seismic_source_t;


typedef struct
{
    int64_t timestamp_us;

    float channel_uv[
        SEISMIC_CHANNEL_COUNT
    ];

} seismic_frame_t;


typedef struct
{
    seismic_source_t source;

    uint32_t sample_rate_hz;

    size_t buffered_frames;

    uint64_t total_frames;

} seismic_buffer_info_t;

typedef struct
{
    size_t peak_occupancy;
    uint64_t dropped_frames_total;
} seismic_buffer_stats_t;


esp_err_t seismic_buffer_init(void);


/* Runtime capacity in frames; zero if initialization/allocation failed. */
size_t seismic_buffer_capacity(void);

esp_err_t seismic_buffer_get_stats(seismic_buffer_stats_t *stats);


/*
 * Call only after producer/consumer tasks are quiesced. In-flight push/copy
 * calls are tracked; configuration then returns ESP_ERR_INVALID_STATE.
 */
esp_err_t seismic_buffer_configure(
    seismic_source_t source,
    uint32_t sample_rate_hz
);


esp_err_t seismic_buffer_push(
    int64_t timestamp_us,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
);


/* Publish with the acquisition-owned index, independent of ring order. */
esp_err_t seismic_buffer_push_indexed(
    int64_t timestamp_us,
    uint64_t source_sample_index,
    const float channel_uv[SEISMIC_CHANNEL_COUNT]
);


esp_err_t seismic_buffer_copy_latest(
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    seismic_buffer_info_t *info
);


/*
 * Copy frames published after *cursor without blocking the producer.
 *
 * cursor is the last consumed frame number. It is advanced only after a
 * consistent batch has been copied. If the consumer falls behind farther than
 * the bounded ring can retain, skipped frames are reported through
 * dropped_frames and copying resumes from the oldest still-published frame.
 *
 * ESP_ERR_INVALID_STATE means the acquisition generation changed (for example
 * stop/start reset total_frames below the cursor). A recorder must treat that
 * as a session boundary rather than mixing independent acquisitions.
 */
esp_err_t seismic_buffer_copy_since(
    uint64_t *cursor,
    seismic_frame_t *frames,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *dropped_frames,
    seismic_buffer_info_t *info
);


/* As copy_since, with acquisition sample indexes for every returned frame. */
esp_err_t seismic_buffer_copy_since_with_source_indices(
    uint64_t *cursor,
    seismic_frame_t *frames,
    uint64_t *source_sample_indices,
    size_t max_frames,
    size_t *frame_count,
    uint64_t *dropped_frames,
    seismic_buffer_info_t *info
);


const char *seismic_buffer_source_name(
    seismic_source_t source
);


#if defined(SEISMIC_BUFFER_TEST_HOOKS)
typedef struct
{
    uint64_t operation_enter_attempts;
    uint64_t operation_cas_retries;
    uint64_t operation_exit_decrements;
    uint64_t capacity_loads;
    uint64_t publish_sequence_loads;
    uint64_t publish_sequence_stores;
    uint64_t read_sequence_loads;
    uint64_t read_retries_odd;
    uint64_t read_retries_changed;
    uint64_t read_tag_conflicts;
    uint64_t metadata_critical_entries;
    uint64_t snapshot_retries;
    uint64_t producer_reader_conflicts;
} seismic_buffer_test_counters_t;

void seismic_buffer_test_counters_reset(void);
void seismic_buffer_test_counters_get(seismic_buffer_test_counters_t *out);
#endif
