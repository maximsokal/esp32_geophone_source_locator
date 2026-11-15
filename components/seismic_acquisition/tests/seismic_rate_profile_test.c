#include <assert.h>
#include <stdio.h>

#include "seismic_rate_profile.h"

static void test_presets_are_the_approved_ones(void)
{
    assert(seismic_rate_profile_count() == 4U);
    assert(seismic_rate_profile_default_hz() == 500U);
    assert(seismic_rate_profile_find(500U)->max_record_seconds == 0U);
    assert(seismic_rate_profile_find(2000U)->max_record_seconds == 120U);
    assert(seismic_rate_profile_find(4000U)->max_record_seconds == 30U);
    assert(seismic_rate_profile_find(5000U)->max_record_seconds == 25U);
    assert(seismic_rate_profile_find(3000U) == NULL);
    assert(seismic_rate_profile_find(0U) == NULL);
    assert(seismic_rate_profile_at(seismic_rate_profile_count()) == NULL);
    for (size_t i = 1U; i < seismic_rate_profile_count(); ++i) {
        assert(seismic_rate_profile_at(i)->sample_rate_hz > seismic_rate_profile_at(i - 1U)->sample_rate_hz);
    }
}

static void test_record_limit_in_milliseconds(void)
{
    assert(seismic_rate_profile_record_limit_ms(500U) == 0U);
    assert(seismic_rate_profile_record_limit_ms(2000U) == 120000U);
    assert(seismic_rate_profile_record_limit_ms(4000U) == 30000U);
    assert(seismic_rate_profile_record_limit_ms(5000U) == 25000U);
}

/* The presets stay inside what the emulator sweep measured: no recording longer than the buffers can absorb. */
static void test_preset_limits_stay_below_the_time_the_buffers_take_to_fill(void)
{
    for (size_t i = 0U; i < seismic_rate_profile_count(); ++i) {
        const seismic_rate_profile_t *p = seismic_rate_profile_at(i);
        if (p->sample_rate_hz <= SEISMIC_RATE_PROFILE_SD_SUSTAINED_FPS) continue;
        uint64_t ring = (uint64_t)p->sample_rate_hz * 2U;
        if (ring > 47662U) ring = 47662U;
        const double fill_s = (double)(26214U + ring) / (double)(p->sample_rate_hz - SEISMIC_RATE_PROFILE_SD_SUSTAINED_FPS);
        assert((double)p->max_record_seconds <= fill_s * 0.8);
    }
}

static void test_rates_that_are_not_presets_are_still_bounded(void)
{
    /* Up to the SD's sustained rate: the 4 kHz limit. Slower than the preview: none. */
    assert(seismic_rate_profile_record_limit_ms(1000U) == 30000U);
    assert(seismic_rate_profile_record_limit_ms(3000U) == 30000U);
    assert(seismic_rate_profile_record_limit_ms(100U) == 0U);
    /* Above it: 3/4 of the time the buffers take to fill, e.g. 10 kHz fills in ~7.7 s -> 5.8 s. */
    const uint32_t at_10k = seismic_rate_profile_record_limit_ms(10000U);
    assert(at_10k >= 5700U && at_10k <= 5800U);
    const uint32_t at_20k = seismic_rate_profile_record_limit_ms(20000U);
    assert(at_20k >= 3000U && at_20k <= 3200U);
    /* Faster is never allowed longer. */
    assert(at_20k < at_10k);
    assert(at_10k < seismic_rate_profile_record_limit_ms(5000U));
    /* Right above the SD rate the formula must not explode or wrap. */
    assert(seismic_rate_profile_record_limit_ms(4001U) >= 1000U);
    assert(seismic_rate_profile_record_limit_ms(40000U) >= 1000U);
}

static void test_preview_decimation(void)
{
    assert(seismic_rate_profile_preview_decimation(100U) == 1U);
    assert(seismic_rate_profile_preview_decimation(500U) == 1U);
    assert(seismic_rate_profile_preview_decimation(2000U) == 4U);
    assert(seismic_rate_profile_preview_decimation(4000U) == 8U);
    assert(seismic_rate_profile_preview_decimation(5000U) == 10U);
    assert(seismic_rate_profile_preview_decimation(20000U) == 40U);
    assert(seismic_rate_profile_preview_decimation(600U) == 2U);   /* never faster than the preview rate */
}

int main(void)
{
    test_presets_are_the_approved_ones();
    test_record_limit_in_milliseconds();
    test_preset_limits_stay_below_the_time_the_buffers_take_to_fill();
    test_rates_that_are_not_presets_are_still_bounded();
    test_preview_decimation();
    puts("seismic_rate_profile_test: OK");
    return 0;
}
