#include <assert.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "localization_channel_health.h"

#define N 4096U
#define CODE_UV 152.6f /* one AD7606 code at the +-5 V range */

static uint32_t rng_state = 12345U;

static float noise_codes(float amplitude_codes)
{
    rng_state = rng_state * 1664525U + 1013904223U;
    return ((float)(rng_state >> 8) / (float)(1U << 24) - 0.5f) * 2.0f * amplitude_codes * CODE_UV;
}

/* A decaying ringing, `swing_codes` at its start, with its own frequency. */
static void ringing(float *out, float swing_codes, float cycles)
{
    for (size_t i = 0U; i < N; ++i) {
        const float t = (float)i / (float)N;
        out[i] = swing_codes * CODE_UV * expf(-3.0f * t) * sinf(6.2831853f * cycles * t);
    }
}

static void add_noise(float *samples, float amplitude_codes)
{
    for (size_t i = 0U; i < N; ++i) samples[i] += noise_codes(amplitude_codes);
}

static float buffers[4][N];

static localization_channel_health_t analyze(const bool present[4])
{
    const float *channels[4];
    for (size_t c = 0U; c < 4U; ++c) channels[c] = present[c] ? buffers[c] : NULL;
    localization_channel_health_t health;
    localization_channel_health_analyze(channels, N, &health);
    return health;
}

static void test_independent_sensors_are_all_clear(void)
{
    ringing(buffers[0], 3000.0f, 7.0f);
    ringing(buffers[1], 2500.0f, 11.0f);
    ringing(buffers[2], 1800.0f, 13.0f);
    ringing(buffers[3], 2200.0f, 17.0f);
    for (size_t c = 0U; c < 4U; ++c) add_noise(buffers[c], 3.0f);
    const bool present[4] = {true, true, true, true};
    const localization_channel_health_t health = analyze(present);
    assert(health.flat_channel_mask == 0U && health.identical_pair_mask == 0U);
}

/* Board log 2026-11-15 20:26: channels 2 and 3 equal within the converter's noise at a 4000-code swing, channels 1 and 4
 * flat at 20 codes. */
static void test_the_board_pattern_names_the_identical_pair_and_both_flat_channels(void)
{
    ringing(buffers[1], 4000.0f, 9.0f);
    for (size_t i = 0U; i < N; ++i) buffers[2][i] = buffers[1][i];
    add_noise(buffers[1], 3.0f);
    add_noise(buffers[2], 3.0f);
    for (size_t i = 0U; i < N; ++i) buffers[0][i] = 0.0f;
    for (size_t i = 0U; i < N; ++i) buffers[3][i] = 0.0f;
    add_noise(buffers[0], 8.0f);
    add_noise(buffers[3], 3.0f);
    const bool present[4] = {true, true, true, true};
    const localization_channel_health_t health = analyze(present);
    size_t a = 9U, b = 9U;
    assert(localization_channel_health_pair(3U, &a, &b) && a == 1U && b == 2U);
    assert(health.identical_pair_mask == (1U << 3));
    assert(health.flat_channel_mask == ((1U << 0) | (1U << 3)));
}

static void test_two_sensors_on_one_event_are_not_identical(void)
{
    /* The same impulse at two places: same shape, different arrival time and strength. */
    ringing(buffers[0], 3000.0f, 9.0f);
    for (size_t i = 0U; i < N; ++i) {
        const size_t from = i >= 40U ? i - 40U : 0U;
        buffers[1][i] = 0.55f * buffers[0][from];
    }
    for (size_t c = 2U; c < 4U; ++c) ringing(buffers[c], 1500.0f, 10.0f + (float)c);
    for (size_t c = 0U; c < 4U; ++c) add_noise(buffers[c], 3.0f);
    const bool present[4] = {true, true, true, true};
    const localization_channel_health_t health = analyze(present);
    assert(health.identical_pair_mask == 0U && health.flat_channel_mask == 0U);
}

/* Two sensors at nearly the same distance see the same ringing at slightly different strength: 20 % is not a short. */
static void test_a_copy_at_another_strength_is_not_identical(void)
{
    ringing(buffers[0], 3000.0f, 9.0f);
    for (size_t i = 0U; i < N; ++i) buffers[1][i] = 0.8f * buffers[0][i];
    ringing(buffers[2], 1500.0f, 12.0f);
    ringing(buffers[3], 1700.0f, 14.0f);
    for (size_t c = 0U; c < 4U; ++c) add_noise(buffers[c], 3.0f);
    const bool present[4] = {true, true, true, true};
    const localization_channel_health_t health = analyze(present);
    assert(health.identical_pair_mask == 0U);
}

static void test_a_quiet_recording_judges_nobody(void)
{
    for (size_t c = 0U; c < 4U; ++c) {
        for (size_t i = 0U; i < N; ++i) buffers[c][i] = 0.0f;
        add_noise(buffers[c], 4.0f);
    }
    /* Equal tiny signals are below the active floor, flat channels need an active neighbour. */
    for (size_t i = 0U; i < N; ++i) buffers[1][i] = buffers[0][i];
    const bool present[4] = {true, true, true, true};
    const localization_channel_health_t health = analyze(present);
    assert(health.flat_channel_mask == 0U && health.identical_pair_mask == 0U);
}

static void test_channels_without_a_sensor_are_not_judged_nor_used_as_reference(void)
{
    ringing(buffers[0], 3000.0f, 7.0f);
    ringing(buffers[2], 2000.0f, 13.0f);
    for (size_t i = 0U; i < N; ++i) buffers[1][i] = 0.0f;
    add_noise(buffers[1], 3.0f);
    for (size_t i = 0U; i < N; ++i) buffers[3][i] = buffers[0][i];
    const bool present[4] = {true, true, true, false};
    localization_channel_health_t health = analyze(present);
    /* Channel 3 (index 3) has no sensor: its copy of channel 0 is not seen. Channel 1 is flat against 0 and 2. */
    assert(health.identical_pair_mask == 0U);
    assert(health.flat_channel_mask == (1U << 1));

    const bool only_one[4] = {true, false, false, false};
    health = analyze(only_one);
    assert(health.flat_channel_mask == 0U && health.identical_pair_mask == 0U);
}

static void test_pair_numbering(void)
{
    static const size_t expected[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
    for (size_t p = 0U; p < 6U; ++p) {
        size_t a = 9U, b = 9U;
        assert(localization_channel_health_pair(p, &a, &b));
        assert(a == expected[p][0] && b == expected[p][1]);
    }
    assert(!localization_channel_health_pair(6U, NULL, NULL));
}

int main(void)
{
    test_independent_sensors_are_all_clear();
    test_the_board_pattern_names_the_identical_pair_and_both_flat_channels();
    test_two_sensors_on_one_event_are_not_identical();
    test_a_copy_at_another_strength_is_not_identical();
    test_a_quiet_recording_judges_nobody();
    test_channels_without_a_sensor_are_not_judged_nor_used_as_reference();
    test_pair_numbering();
    puts("localization_channel_health_test: OK");
    return 0;
}
