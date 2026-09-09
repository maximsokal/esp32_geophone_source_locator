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
}


static void test_suspicious_streak_and_recovery(void)
{
    uint8_t all_ff[AD7606_SERIAL_RX_BYTES];
    memset(all_ff, 0xFF, sizeof(all_ff));

    ad7606_serial_diag_state_t state;
    ad7606_serial_diag_reset(&state);

    for (uint32_t frame = 0U; frame < AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD; ++frame) {
        ad7606_serial_diag_event_t event;
        assert(ad7606_serial_diag_observe(&state, all_ff, &event));
        if (frame == 0U) {
            assert(event.pattern_changed);
            assert(event.abnormal_started);
        }
    }

    assert(state.serial_suspicious);
    assert(state.all_ff_frames == AD7606_SERIAL_SUSPICIOUS_STREAK_THRESHOLD);

    uint8_t varied[AD7606_SERIAL_RX_BYTES] = {0};
    for (size_t index = 0U; index < AD7606_SERIAL_WORD_COUNT; ++index) {
        put_word(varied, index, (uint16_t)(0x1000U + index));
    }
    ad7606_serial_diag_event_t recovery_event;
    assert(ad7606_serial_diag_observe(&state, varied, &recovery_event));
    assert(recovery_event.pattern_changed);
    assert(!state.serial_suspicious);
    assert(state.varied_frames == 1U);
}


int main(void)
{
    test_big_endian_signed_decode();
    test_frame_classification();
    test_suspicious_streak_and_recovery();
    puts("AD7606 serial diagnostics tests passed");
    return 0;
}
