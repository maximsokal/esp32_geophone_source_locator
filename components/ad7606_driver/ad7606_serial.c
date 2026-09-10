#include "ad7606_serial.h"

#include <limits.h>
#include <string.h>


static uint32_t ad7606_serial_saturating_increment(
    uint32_t value
)
{
    return value == UINT32_MAX ? value : value + 1U;
}

static bool ad7606_serial_has_clock_like_rail_pattern(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    const int16_t words[AD7606_SERIAL_WORD_COUNT]
)
{
    bool clock_like_byte = false;
    unsigned positive_rail_words = 0U;
    unsigned negative_rail_words = 0U;
    for (size_t i = 0U; i < AD7606_SERIAL_RX_BYTES; ++i) {
        const uint8_t byte = rx[i];
        clock_like_byte = clock_like_byte ||
            byte == 0xAAU || byte == 0x55U || byte == 0x2AU;
    }
    for (size_t channel = 0U; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        if (words[channel] >= 30000) ++positive_rail_words;
        else if (words[channel] <= -30000) ++negative_rail_words;
    }
    return clock_like_byte && positive_rail_words > 0U && negative_rail_words > 0U;
}


int16_t ad7606_serial_decode_word(
    const uint8_t bytes[AD7606_SERIAL_WORD_BYTES]
)
{
    if (bytes == NULL) {
        return 0;
    }

    const uint16_t word =
        ((uint16_t)bytes[0] << 8) |
        (uint16_t)bytes[1];

    return (int16_t)word;
}


bool ad7606_serial_decode_frame(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    int16_t words[AD7606_SERIAL_WORD_COUNT]
)
{
    if (rx == NULL || words == NULL) {
        return false;
    }

    for (size_t channel = 0; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        words[channel] = ad7606_serial_decode_word(
            &rx[channel * AD7606_SERIAL_WORD_BYTES]
        );
    }

    return true;
}


ad7606_serial_pattern_t ad7606_serial_classify(
    const uint8_t rx[AD7606_SERIAL_RX_BYTES]
)
{
    if (rx == NULL) {
        return AD7606_SERIAL_PATTERN_UNKNOWN;
    }

    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < AD7606_SERIAL_RX_BYTES; ++i) {
        all_zero = all_zero && rx[i] == 0x00U;
        all_ff = all_ff && rx[i] == 0xFFU;
    }

    if (all_zero) {
        return AD7606_SERIAL_PATTERN_ALL_00;
    }

    if (all_ff) {
        return AD7606_SERIAL_PATTERN_ALL_FF;
    }

    int16_t words[AD7606_SERIAL_WORD_COUNT];
    if (!ad7606_serial_decode_frame(rx, words)) {
        return AD7606_SERIAL_PATTERN_UNKNOWN;
    }

    bool all_words_identical = true;
    for (size_t channel = 1; channel < AD7606_SERIAL_WORD_COUNT; ++channel) {
        if (words[channel] != words[0]) {
            all_words_identical = false;
            break;
        }
    }

    if (all_words_identical) return AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL;
    return ad7606_serial_has_clock_like_rail_pattern(rx, words)
        ? AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE
        : AD7606_SERIAL_PATTERN_VARIED;
}


const char *ad7606_serial_pattern_name(
    ad7606_serial_pattern_t pattern
)
{
    switch (pattern) {
        case AD7606_SERIAL_PATTERN_ALL_00:
            return "ALL_00";

        case AD7606_SERIAL_PATTERN_ALL_FF:
            return "ALL_FF";

        case AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL:
            return "ALL_WORDS_IDENTICAL";

        case AD7606_SERIAL_PATTERN_VARIED:
            return "VARIED";

        case AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE:
            return "DIGITAL_INTERFERENCE";

        case AD7606_SERIAL_PATTERN_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


void ad7606_serial_diag_reset(
    ad7606_serial_diag_state_t *state
)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->pattern = AD7606_SERIAL_PATTERN_UNKNOWN;
}


bool ad7606_serial_diag_observe(
    ad7606_serial_diag_state_t *state,
    const uint8_t rx[AD7606_SERIAL_RX_BYTES],
    ad7606_serial_diag_event_t *event
)
{
    if (state == NULL || rx == NULL) {
        return false;
    }

    ad7606_serial_diag_event_t local_event = {0};
    if (event == NULL) {
        event = &local_event;
    }
    *event = (ad7606_serial_diag_event_t){0};

    const ad7606_serial_pattern_t pattern =
        ad7606_serial_classify(rx);
    const bool repeated_frame =
        state->previous_frame_valid &&
        memcmp(
            state->previous_rx,
            rx,
            AD7606_SERIAL_RX_BYTES
        ) == 0;

    event->pattern_changed =
        !state->initialized ||
        state->pattern != pattern;

    const bool abnormal_frame =
        pattern != AD7606_SERIAL_PATTERN_VARIED ||
        repeated_frame;

    const uint32_t old_suspicious_streak =
        state->suspicious_streak;
    const bool old_suspicious =
        state->serial_suspicious;

    state->pattern = pattern;
    state->initialized = true;
    state->last_frame_repeated = repeated_frame;

    if (pattern == AD7606_SERIAL_PATTERN_ALL_00) {
        state->all_zero_frames =
            ad7606_serial_saturating_increment(state->all_zero_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_FF) {
        state->all_ff_frames =
            ad7606_serial_saturating_increment(state->all_ff_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL) {
        state->all_identical_frames =
            ad7606_serial_saturating_increment(state->all_identical_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_VARIED) {
        state->varied_frames =
            ad7606_serial_saturating_increment(state->varied_frames);
    }
    else if (pattern == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE) {
        state->digital_interference_frames =
            ad7606_serial_saturating_increment(state->digital_interference_frames);
    }

    if (repeated_frame) {
        state->repeated_frame_streak =
            ad7606_serial_saturating_increment(state->repeated_frame_streak);
    }
    else {
        state->repeated_frame_streak = 0U;
    }

    if (abnormal_frame) {
        state->suspicious_streak =
            ad7606_serial_saturating_increment(state->suspicious_streak);
    }
    else {
        state->suspicious_streak = 0U;
    }

    state->serial_suspicious =
        state->suspicious_streak >=
        AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD;

    event->abnormal_started =
        abnormal_frame &&
        old_suspicious_streak == 0U;
    event->suspicious_started =
        !old_suspicious &&
        state->serial_suspicious;

    memcpy(
        state->previous_rx,
        rx,
        AD7606_SERIAL_RX_BYTES
    );
    state->previous_frame_valid = true;

    return true;
}
