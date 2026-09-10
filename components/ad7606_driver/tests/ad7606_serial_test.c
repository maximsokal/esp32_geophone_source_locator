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
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_CLIPPED);
    assert(state.quality_window_digital_interference_frames == 50U);
    assert(state.quality_window_bad_frames == 50U);
    assert(state.signal_quality_window_rail_hit_frames == 50U);
    assert(state.signal_quality_window_clip_frames == 50U);
    assert(state.quality_window_frames == 500U);
    assert(state.stream_state == AD7606_SERIAL_STREAM_SUSPICIOUS);
    assert(!state.stream_stuck);

    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x3000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_bad_frames == 0U);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_HEALTHY);
    assert(state.signal_quality_window_rail_hit_frames == 0U);
    assert(state.signal_quality_window_clip_frames == 0U);
    assert(state.stream_state == AD7606_SERIAL_STREAM_NORMAL);
    assert(!state.stream_stuck);

    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x4000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(ad7606_serial_diag_observe(&state, interference, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_CLIPPED);
    assert(state.signal_quality_window_clip_frames == 1U);

    ad7606_serial_diag_reset(&state);
    uint8_t all_ff[AD7606_SERIAL_RX_BYTES];
    memset(all_ff, 0xFF, sizeof(all_ff));
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD; ++frame) {
        assert(ad7606_serial_diag_observe(&state, all_ff, NULL));
    }
    assert(!state.serial_suspicious);
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(ad7606_serial_diag_observe(&state, varied, NULL));
    assert(!state.serial_suspicious);
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_HEALTHY);
    assert(state.quality_window_all_ff_frames == 10U);
    assert(state.quality_window_repeated_frames == 9U);
    /* Stream plausibility waits for a complete window before deciding. */
    assert(state.stream_state == AD7606_SERIAL_STREAM_UNKNOWN);
    assert(!state.stream_stuck);
}


static void test_quiet_frames_are_not_transport_faults(void)
{
    uint8_t all_zero[AD7606_SERIAL_RX_BYTES] = {0};
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x2000U + index));
    }

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);

    /* A sustained ALL_00 observation is still a diagnostic, not a transport fault. */
    for (uint32_t frame = 0U; frame < 999U; ++frame) {
        assert(ad7606_serial_diag_observe(&state, all_zero, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_all_zero_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES);
    assert(state.quality_window_bad_frames == 0U);
    assert(state.quality_window_digital_interference_frames == 0U);
    assert(state.stream_state == AD7606_SERIAL_STREAM_QUIET);
    assert(!state.stream_stuck);
    assert(ad7606_serial_diag_observe(&state, varied, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_all_zero_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES - 1U);

    ad7606_serial_diag_reset(&state);
    uint8_t all_ff[AD7606_SERIAL_RX_BYTES];
    memset(all_ff, 0xFF, sizeof(all_ff));
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        assert(ad7606_serial_diag_observe(&state, all_ff, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_all_ff_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES);
    assert(state.quality_window_repeated_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES - 1U);
    assert(state.stream_state == AD7606_SERIAL_STREAM_STUCK);
    assert(state.stream_stuck);
}


static void test_persistent_identical_stream_is_stuck(void)
{
    uint8_t identical[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(identical, index, 0x1234U);
    }

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        assert(ad7606_serial_diag_observe(&state, identical, NULL));
    }

    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_all_identical_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES);
    assert(state.quality_window_repeated_frames == AD7606_SERIAL_QUALITY_WINDOW_FRAMES - 1U);
    assert(state.stream_state == AD7606_SERIAL_STREAM_STUCK);
    assert(state.stream_stuck);
}


static void test_signal_quality_is_separate_from_transport(void)
{
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x1200U + index));
    }

    uint8_t rail[AD7606_SERIAL_RX_BYTES] = {0};
    put_word(rail, 0U, 0x7FFFU);
    put_word(rail, 1U, 0x7531U);
    put_word(rail, 2U, 0x8ACFU);
    put_word(rail, 3U, 0x1234U);
    put_word(rail, 4U, 0x2345U);
    put_word(rail, 5U, 0x3456U);
    put_word(rail, 6U, 0x4567U);
    put_word(rail, 7U, 0x5678U);
    assert(ad7606_serial_classify(rail) == AD7606_SERIAL_PATTERN_VARIED);

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x1200U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(ad7606_serial_diag_observe(&state, rail, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_CLIPPED);
    assert(state.signal_quality_window_rail_hit_frames == 1U);
    assert(state.signal_quality_window_clip_frames == 1U);

    /* A clipped diagnostic-only CH7 word must not contaminate CH1-CH4 signal quality. */
    uint8_t diagnostic_only_clip[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(diagnostic_only_clip, index, (uint16_t)(0x1600U + index));
    }
    put_word(diagnostic_only_clip, 6U, 0x7FFFU);
    assert(ad7606_serial_classify(diagnostic_only_clip) == AD7606_SERIAL_PATTERN_VARIED);

    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x1600U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(ad7606_serial_diag_observe(&state, diagnostic_only_clip, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_HEALTHY);
    assert(state.signal_quality_window_rail_hit_frames == 0U);
    assert(state.signal_quality_window_clip_frames == 0U);

    /* A saturated analog frame may contain two clock-like bytes by chance;
     * it remains a signal observation, not a transport fault. */
    uint8_t saturated_with_two_clock_bytes[AD7606_SERIAL_RX_BYTES] = {0};
    put_word(saturated_with_two_clock_bytes, 0U, 0x7FFFU);
    put_word(saturated_with_two_clock_bytes, 1U, 0x55AAU);
    put_word(saturated_with_two_clock_bytes, 2U, 30000U);
    put_word(saturated_with_two_clock_bytes, 3U, (uint16_t)-30000);
    put_word(saturated_with_two_clock_bytes, 4U, 0x2345U);
    put_word(saturated_with_two_clock_bytes, 5U, 0x3456U);
    put_word(saturated_with_two_clock_bytes, 6U, 0x4567U);
    put_word(saturated_with_two_clock_bytes, 7U, 0x5678U);
    assert(
        ad7606_serial_classify(saturated_with_two_clock_bytes) ==
        AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE
    );

    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x1800U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    assert(ad7606_serial_diag_observe(&state, saturated_with_two_clock_bytes, NULL));
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_CLIPPED);
    assert(state.quality_window_digital_interference_frames == 0U);

    uint8_t near_rail[AD7606_SERIAL_RX_BYTES] = {0};
    put_word(near_rail, 0U, 30000U);
    put_word(near_rail, 1U, (uint16_t)-30000);
    put_word(near_rail, 2U, 0x1234U);
    put_word(near_rail, 3U, 0x2345U);
    put_word(near_rail, 4U, 0x3456U);
    put_word(near_rail, 5U, 0x4567U);
    put_word(near_rail, 6U, 0x5678U);
    put_word(near_rail, 7U, 0x6789U);
    assert(ad7606_serial_classify(near_rail) == AD7606_SERIAL_PATTERN_VARIED);

    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U; frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES; ++frame) {
        put_word(varied, 7U, (uint16_t)(0x2200U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }
    for (uint32_t frame = 0U; frame < 10U; ++frame) {
        assert(ad7606_serial_diag_observe(&state, near_rail, NULL));
    }
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_DEGRADED);
    assert(state.signal_quality_window_rail_hit_frames == 10U);
    assert(state.signal_quality_window_clip_frames == 0U);
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
    assert(state.stream_state == AD7606_SERIAL_STREAM_SUSPICIOUS);
    assert(!state.stream_stuck);
    assert(state.signal_quality == AD7606_SIGNAL_QUALITY_CLIPPED);
    assert(state.signal_quality_window_clip_frames == 10U);
}


static void test_scattered_clock_like_payload_stays_diagnostic_only(void)
{
    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x5000U + index));
    }

    /* This is the supplied real-log shape: clock-like bytes are scattered,
     * not a contiguous wire-clock signature. */
    const uint8_t scattered[AD7606_SERIAL_RX_BYTES] = {
        0xFFU, 0x80U, 0xABU, 0xFCU, 0x51U, 0xD5U, 0x55U, 0xF8U,
        0xAAU, 0xA8U, 0x05U, 0x55U, 0x02U, 0xEAU, 0xAAU, 0xB4U,
    };

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);
    for (uint32_t frame = 0U;
         frame < AD7606_SERIAL_QUALITY_WINDOW_FRAMES;
         ++frame) {
        put_word(varied, 7U, (uint16_t)(0x5000U + frame));
        assert(ad7606_serial_diag_observe(&state, varied, NULL));
    }

    assert(ad7606_serial_diag_observe(&state, scattered, NULL));
    assert(state.pattern == AD7606_SERIAL_PATTERN_DIGITAL_INTERFERENCE);
    assert(state.digital_interference_frames == 1U);
    assert(state.quality == AD7606_SERIAL_QUALITY_HEALTHY);
    assert(!state.serial_suspicious);
    assert(state.quality_window_digital_interference_frames == 0U);
    assert(state.quality_window_bad_frames == 0U);
}


int main(void)
{
    test_big_endian_signed_decode();
    test_frame_classification();
    test_real_log_frames_are_not_plain_varied();
    test_quality_window_hysteresis();
    test_quiet_frames_are_not_transport_faults();
    test_persistent_identical_stream_is_stuck();
    test_signal_quality_is_separate_from_transport();
    test_quality_window_degraded_band();
    test_scattered_clock_like_payload_stays_diagnostic_only();
    puts("AD7606 serial diagnostics tests passed");
    return 0;
}
