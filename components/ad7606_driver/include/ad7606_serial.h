#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define AD7606_SERIAL_WORD_COUNT        8U
#define AD7606_SERIAL_WORD_BYTES        2U
#define AD7606_SERIAL_RX_BYTES          \
    (AD7606_SERIAL_WORD_COUNT * AD7606_SERIAL_WORD_BYTES)

/* A single abnormal frame is not enough to declare the serial path broken. */
#define AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD  10U


typedef enum
{
    AD7606_SERIAL_PATTERN_UNKNOWN = 0,
    AD7606_SERIAL_PATTERN_ALL_00,
    AD7606_SERIAL_PATTERN_ALL_FF,
    AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL,
    AD7606_SERIAL_PATTERN_VARIED,
    AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE

} ad7606_serial_pattern_t;


typedef struct
{
    bool initialized;
    bool previous_frame_valid;
    bool last_frame_repeated;
    bool serial_suspicious;

    ad7606_serial_pattern_t pattern;

    uint32_t suspicious_streak;
    uint32_t repeated_frame_streak;
    uint32_t all_zero_frames;
    uint32_t all_ff_frames;
    uint32_t all_identical_frames;
    uint32_t varied_frames;
    uint32_t digital_interference_frames;

    uint8_t previous_rx[AD7606_SERIAL_RX_BYTES];

} ad7606_serial_diag_state_t;


typedef struct
{
    bool pattern_changed;
    bool abnormal_started;
    bool suspicious_started;

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


void ad7606_serial_diag_reset(
    ad7606_serial_diag_state_t *state
);


bool ad7606_serial_diag_observe(
    ad7606_serial_diag_state_t *state,
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    ad7606_serial_diag_event_t *event
);
