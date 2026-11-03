#define main cs_quality_baseline_main
#include "nrf_cs_measurement_quality_test.c"
#undef main

#include <float.h>
#include <stdio.h>

static void test_confidence_validity_and_number_classes(void)
{
    const float values[] = {NAN, INFINITY, -INFINITY, 0.0f, 1.0f, FLT_MAX};
    for (unsigned flags = 0U; flags < 4U; ++flags) {
        for (unsigned pbr = 0U; pbr < 6U; ++pbr) {
            for (unsigned rtt = 0U; rtt < 6U; ++rtt) {
                const bool pbr_valid = (flags & 1U) != 0U;
                const bool rtt_valid = (flags & 2U) != 0U;
                const uint16_t confidence = nrf_cs_measurement_confidence(
                    45U, 15U, pbr_valid, rtt_valid, values[pbr], values[rtt]);
                assert(confidence <= NRF_CS_CONFIDENCE_MAX);
                if ((pbr_valid && !isfinite(values[pbr])) ||
                    (rtt_valid && !isfinite(values[rtt])) || flags == 0U) {
                    assert(confidence == 0U);
                } else if (!pbr_valid) {
                    assert(confidence == 400U);
                } else if (!rtt_valid) {
                    assert(confidence == 400U);
                } else if (pbr == rtt) {
                    assert(confidence == 1000U);
                }
            }
        }
    }
    puts("SIGNAL_CASE CS.VALID_NUMBERS PASS flag_sets=4 number_classes=6");
}

static void test_confidence_every_tone_count(void)
{
    const uint16_t thresholds[] = {0U, 1U, 15U, UINT16_MAX};
    for (unsigned threshold = 0U; threshold < 4U; ++threshold) {
        uint16_t previous = 0U;
        for (uint32_t tones = 0U; tones <= UINT16_MAX; ++tones) {
            const uint16_t result = nrf_cs_measurement_confidence(
                (uint16_t)tones, thresholds[threshold], true, false, 3.0f, NAN);
            assert(result >= previous && result <= NRF_CS_CONFIDENCE_MAX);
            if (thresholds[threshold] == 0U || tones < thresholds[threshold]) assert(result == 0U);
            else assert(result >= 240U && result <= 400U);
            previous = result;
        }
    }
    puts("SIGNAL_CASE CS.TONES PASS counts=65536 thresholds=4");
}

int main(void)
{
    assert(cs_quality_baseline_main() == 0);
    test_confidence_validity_and_number_classes();
    test_confidence_every_tone_count();
    return 0;
}
