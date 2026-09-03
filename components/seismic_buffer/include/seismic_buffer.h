#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"


#define SEISMIC_CHANNEL_COUNT    4
#define SEISMIC_BUFFER_CAPACITY  2048


typedef enum
{
    SEISMIC_SOURCE_NONE = 0,
    SEISMIC_SOURCE_TEST,
    SEISMIC_SOURCE_ESP_ADC_BENCH,
    SEISMIC_SOURCE_AD7606

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


esp_err_t seismic_buffer_init(void);


esp_err_t seismic_buffer_configure(
    seismic_source_t source,
    uint32_t sample_rate_hz
);


esp_err_t seismic_buffer_push(
    int64_t timestamp_us,
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


const char *seismic_buffer_source_name(
    seismic_source_t source
);
