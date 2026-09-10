#include "ad7606_serial.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


static void put_word(
    uint8_t rx[AD7606_SERIAL_RX_BYTES],
    size_t index,
    uint16_t word
)
{
    rx[index * 2U] = (uint8_t)(word >> 8);
    rx[index * 2U + 1U] = (uint8_t)word;
}


static void test_big_endian_signed_decode(void)
{
    const uint8_t zero[] = {0x00U, 0x00U};
    const uint8_t minus_one[] = {0xFFU, 0xFFU};
    const uint8_t max[] = {0x7FU, 0xFFU};
    const uint8_t min[] = {0x80U, 0x00U};
    const uint8_t pattern[] = {0x12U, 0x34U};

    assert(ad7606_serial_decode_word(zero) == 0);
    assert(ad7606_serial_decode_word(minus_one) == -1);
    assert(ad7606_serial_decode_word(max) == 32767);
    assert(ad7606_serial_decode_word(min) == -32768);
    assert(ad7606_serial_decode_word(pattern) == 0x1234);

    uint8_t rx[AD7606_SERIAL_RX_BYTES] = {0};
    int16_t words[AD7606_SERIAL_WORD_COUNT] = {0};
    put_word(rx, 0U, 0x0000U);
    put_word(rx, 1U, 0xFFFFU);
    put_word(rx, 2U, 0x7FFFU);
    put_word(rx, 3U, 0x8000U);
    put_word(rx, 4U, 0x1234U);
    put_word(rx, 5U, 0x0001U);
    put_word(rx, 6U, 0xFFFEU);
    put_word(rx, 7U, 0x4000U);

    assert(ad7606_serial_decode_frame(rx, words));
    assert(words[0] == 0);
    assert(words[1] == -1);
    assert(words[2] == 32767);
    assert(words[3] == -32768);
    assert(words[4] == 0x1234);
    assert(words[5] == 1);
    assert(words[6] == -2);
    assert(words[7] == 0x4000);
}


static void test_frame_classification(void)
{
    uint8_t rx[AD7606_SERIAL_RX_BYTES] = {0};
    assert(ad7606_serial_classify(rx) == AD7606_SERIAL_PATTERN_ALL_00);

    memset(rx, 0xFF, sizeof(rx));
    assert(ad7606_serial_classify(rx) == AD7606_SERIAL_PATTERN_ALL_FF);

    for (size_t index = 0; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(rx, index, 0x1234U);
    }
    assert(ad7606_serial_classify(rx) == AD7606_SERIAL_PATTERN_ALL_WORDS_IDENTICAL);

    put_word(rx, 7U, 0x1235U);
    assert(ad7606_serial_classify(rx) == AD7606_SERIAL_PATTERN_VARIED);
    assert(strcmp(ad7606_serial_pattern_name(AD7606_SERIAL_PATTERN_ALL_FF), "ALL_FF") == 0);

    put_word(rx, 0U, 0x7FFFU);
    put_word(rx, 1U, 0x8000U);
    put_word(rx, 2U, 0x55AAU);
    put_word(rx, 3U, 0x2A55U);
    assert(ad7606_serial_classify(rx) == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE);
    assert(strcmp(ad7606_serial_pattern_name(AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE), "DIGITAL_INTERFERENCE") == 0);
}


static void test_real_log_frames_are_not_plain_varied(void)
{
    uint8_t frame_one[AD7606_SERIAL_RX_BYTES] = {0};
    const uint16_t words_one[AD7606_SERIAL_WORD_COUNT] = {
        31360U, 1364U, (uint16_t)-28640, 10752U,
        5376U, 250U, 0U, 21760U
    };
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(frame_one, index, words_one[index]);
    }
    assert(ad7606_serial_classify(frame_one) == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE);

    const uint8_t frame_two[AD7606_SERIAL_RX_BYTES] = {
        0xFFU, 0x80U, 0xABU, 0xFCU, 0x51U, 0xD5U, 0x55U, 0xF8U,
        0xAAU, 0xA8U, 0x05U, 0x55U, 0x02U, 0xEAU, 0xAAU, 0xB4U
    };
    assert(ad7606_serial_classify(frame_two) == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE);
}


static void test_quality_window_hysteresis(void)
{
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x1000U + index));
    }

    uint8_t interference[AD7606_SERIAL_RX_BYTES] = {0};
    put_word(interference, 0U, 0x7FFFU);
    put_word(interference, 1U, 0x8000U);
    put_word(interference, 2U, 0x55AAU);
    put_word(interference, 3U, 0x2A55U);

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);

    for (uint32_t frame = 0U; frame < 400U; ++frame) {
        ad7606_serial_diag_event_t event;
        put_word(varied, 7U, (uint16_t)(0x1000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, &event));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);

    for (uint32_t frame = 0U; frame < 50U; ++frame) {
        assert(ad7606_serial_diag_observe(&state, interference, NULL));
        put_word(varied, 7U, (uint16_t)(0x2000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_FAULT);
    assert(state.serial_suspicious);
    assert(state.quality_window_digital_interference_frames == 50U);
    assert(state.quality_window_bad_frames == 50U);
    assert(state.quality_window_frames == 500U);

    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x3000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_bad_frames == 0U);

    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x4000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(ad7606_serial_diag_observe(&state, interference, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);

    ad7606_serial_diag_reset(&state);
    uint8_t all_ff[AD7606_SERIAL_RX_BYTES];
    memset(all_ff, 0xFF, sizeof(all_ff));
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD; ++frame) {
        assert(ad7606_serial_diag_observe(&state, all_ff, NULL));
    }
    assert(state.serial_suspicious);
    assert(state.quality == AD7606_SERIAL_QUALITY_FAULT);
    assert(ad7606_serial_diag_observe(&state, varied, NULL));
    assert(state.serial_suspicious);
    assert(state.quality_window_all_ff_frames == 10U);
    assert(state.quality_window_repeated_frames == 9U);
}


static void test_quality_window_all_zero_persistence(void)
{
    uint8_t all_zero[AD7606_SERIAL_RX_BYTES] = {0};
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x2000U + index));
    }

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);

    /* A single varied frame must not clear a sustained ALL_00 fault. */
    for (uint32_t frame = 0U; frame < 999U; ++frame) {
        assert(ad7606_serial_diag_observe(&state, all_zero, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_FAULT);
    assert(state.quality_window_all_zero_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES);
    assert(ad7606_serial_diag_observe(&state, varied, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_FAULT);
    assert(state.serial_suspicious);
}


static void test_quality_window_degraded_band(void)
{
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x3000U + index));
    }

    uint8_t interference[AD7606_SERIAL_RX_BYTES] = {0};
    put_word(interference, 0U, 0x7FFFU);
    put_word(interference, 1U, 0x8000U);
    put_word(interference, 2U, 0x55AAU);
    put_word(interference, 3U, 0x2A55U);

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < 490U; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x3000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    for (uint32_t frame = 0U; frame < 10U; ++frame) {
        assert(ad7606_serial_diag_observe(&state, interference, NULL));
    }

    assert(state.quality == AD7606_SERIAL_QUALITY_DEGRADED);
    assert(state.quality_window_bad_frames == 10U);
    assert(state.quality_window_digital_interference_frames == 10U);
    assert(state.serial_suspicious);
}


int main(void)
{
    test_big_endian_signed_decode();
    test_frame_classification();
    test_real_log_frames_are_not_plain_varied();
    test_quality_window_hysteresis();
    test_quality_window_all_zero_persistence();
    test_quality_window_degraded_band();
    puts("AD7606 serial diagnostics tests passed");
    return 0;
}
