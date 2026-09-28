#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define AD7606_SERIAL_WORD_COUNT        4U
#define AD7606_SERIAL_WORD_BYTES        2U
#define AD7606_SERIAL_RX_BYTES          \
    (AD7606_SERIAL_WORD_COUNT * AD7606_SERIAL_WORD_BYTES)

/* The application waveform and serial frame contain only used inputs V1-V4. */
#define AD7606_SIGNAL_CHANNEL_COUNT     4U

#if AD7606_SIGNAL_CHANNEL_COUNT == 0U || \
    AD7606_SIGNAL_CHANNEL_COUNT > AD7606_SERIAL_WORD_COUNT
#error "AD7606 signal channel scope must fit inside the serial frame"
#endif

#define AD7606_SERIAL_QUALITY_WINDOW_FRAMES  500U /* standalone/test default */
/* Keep the state UNKNOWN until a short initial sample is available. */
#define AD7606_SERIAL_QUALITY_MIN_FRAMES     10U

/* A single transport anomaly is not enough to declare the serial path broken. */
#define AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD  10U

#define AD7606_SERIAL_STREAM_STUCK_ENTER_PERCENT   80U
#define AD7606_SERIAL_STREAM_STUCK_LEAVE_PERCENT   60U
#define AD7606_SERIAL_STREAM_QUIET_ENTER_PERCENT   80U
#define AD7606_SERIAL_STREAM_QUIET_LEAVE_PERCENT   60U


typedef enum
{
    AD7606_SERIAL_PATTERN_UNKNOWN = 0,
    AD7606_SERIAL_PATTERN_ALL_00,
    AD7606_SERIAL_PATTERN_ALL_FF,
    AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL,
    AD7606_SERIAL_PATTERN_VARIED,
    AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE

} ad7606_serial_pattern_t;


typedef enum
{
    /* Transport quality: this state excludes quiet/flat analog observations. */
    AD7606_SERIAL_QUALITY_UNKNOWN = 0,
    AD7606_SERIAL_QUALITY_HEALTHY,
    AD7606_SERIAL_QUALITY_DEGRADED,
    AD7606_SERIAL_QUALITY_FAULT

} ad7606_serial_quality_t;


typedef enum
{
    /* Signal quality: near-rail and hard-clipped analog samples only. */
    AD7606_SIGNAL_QUALITY_UNKNOWN = 0,
    AD7606_SIGNAL_QUALITY_HEALTHY,
    AD7606_SIGNAL_QUALITY_DEGRADED,
    AD7606_SIGNAL_QUALITY_CLIPPED

} ad7606_signal_quality_t;


typedef enum
{
    /* Data plausibility is separate from SPI transport quality. */
    AD7606_SERIAL_STREAM_UNKNOWN = 0,
    AD7606_SERIAL_STREAM_NORMAL,
    AD7606_SERIAL_STREAM_QUIET,
    AD7606_SERIAL_STREAM_STUCK,
    AD7606_SERIAL_STREAM_SUSPICIOUS

} ad7606_serial_stream_state_t;


typedef struct
{
    bool initialized;
    bool previous_frame_valid;
    bool last_frame_repeated;
    bool serial_suspicious;

    ad7606_serial_pattern_t pattern;
    ad7606_serial_quality_t quality;
    ad7606_signal_quality_t signal_quality;
    ad7606_serial_stream_state_t stream_state;
    bool stream_stuck;

    uint32_t suspicious_streak;
    uint32_t repeated_frame_streak;
    uint32_t all_zero_frames;
    uint32_t all_ff_frames;
    uint32_t all_identical_frames;
    uint32_t varied_frames;
    uint32_t digital_interference_frames;
    /* Signal-domain near-rail and hard-clip observations. */
    uint32_t signal_rail_hit_frames;
    uint32_t signal_clip_frames;

    uint32_t quality_window_frames;
    uint32_t quality_window_bad_frames;
    uint32_t quality_window_all_zero_frames;
    uint32_t quality_window_all_ff_frames;
    uint32_t quality_window_all_identical_frames;
    uint32_t quality_window_repeated_frames;
    uint32_t quality_window_digital_interference_frames;
    uint32_t signal_quality_window_rail_hit_frames;
    uint32_t signal_quality_window_clip_frames;
    uint32_t quality_window_next;
    uint32_t quality_window_capacity;
    uint8_t *quality_window_flags;

    uint8_t previous_rx[AD7606_SERIAL_RX_BYTES];

} ad7606_serial_diag_state_t;


typedef struct
{
    bool pattern_changed;
    bool abnormal_started;
    bool suspicious_started;
    bool quality_changed;
    bool signal_quality_changed;
    bool stream_state_changed;

} ad7606_serial_diag_event_t;


int16_t ad7606_serial_decode_word(
    const uint8_t bytes[AD7606_SERIAL_WORD_BYTES]
);


bool ad7606_serial_decode_frame(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    int16_t words[AD7606_SERIAL_WORD_COUNT]
);


ad7606_serial_pattern_t ad7606_serial_classify(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES]
);


const char *ad7606_serial_pattern_name(
    ad7606_serial_pattern_t pattern
);


const char *ad7606_serial_quality_name(
    ad7606_serial_quality_t quality
);


const char *ad7606_signal_quality_name(
    ad7606_signal_quality_t quality
);


const char *ad7606_serial_stream_state_name(
    ad7606_serial_stream_state_t state
);


void ad7606_serial_diag_reset(
    ad7606_serial_diag_state_t *state,
    uint8_t *quality_window_flags,
    uint32_t quality_window_capacity
);

uint32_t ad7606_serial_quality_window_frames(uint32_t sample_rate_hz);


bool ad7606_serial_diag_observe(
    ad7606_serial_diag_state_t *state,
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    ad7606_serial_diag_event_t *event
);
