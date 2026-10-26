#define main ad7606_baseline_main
#include "../../components/ad7606_driver/tests/ad7606_driver_host_test.c"
#undef main

#include <math.h>
#include <stdio.h>

static void test_all_adc_words_channels_and_scale_commands(void)
{
    const uint8_t os[] = {1U, 2U, 4U, 8U};
    const uint32_t ranges[] = {5000U, 10000U};
    for (int32_t code = INT16_MIN; code <= INT16_MAX; ++code) {
        for (unsigned channel = 0U; channel < 4U; ++channel) {
            uint8_t frame[8] = {0};
            frame[channel * 2U] = (uint8_t)((uint16_t)code >> 8U);
            frame[channel * 2U + 1U] = (uint8_t)code;
            int16_t words[4];
            assert(ad7606_serial_decode_frame(frame, words));
            for (unsigned other = 0U; other < 4U; ++other) {
                assert(words[other] == (other == channel ? code : 0));
            }
        }
        for (unsigned o = 0U; o < 4U; ++o) {
            for (unsigned r = 0U; r < 2U; ++r) {
                float actual;
                assert(ad7606_raw_to_uv_for_config((int16_t)code, os[o], ranges[r], &actual));
                const float expected = (float)((double)code * (double)ranges[r] * 1000.0 / 32768.0);
                assert(isfinite(actual) && fabsf(actual - expected) <= 0.5f);
            }
        }
    }
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        float result = 123.0f;
        const bool allowed = byte == 1U || byte == 2U || byte == 4U || byte == 8U;
        assert(ad7606_raw_to_uv_for_config(1, (uint8_t)byte, 5000U, &result) == allowed);
        if (!allowed) assert(result == 123.0f);
    }
    puts("SIGNAL_CASE AD7606.WORDS_COMMANDS PASS words=65536 channels=4 ranges=2 os=256");
}

int main(void)
{
    assert(ad7606_baseline_main() == 0);
    test_all_adc_words_channels_and_scale_commands();
    return 0;
}
