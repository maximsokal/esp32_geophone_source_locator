#include <assert.h>
#include <stdint.h>

#include "nrf_height_reference.h"

static uint32_t next_fixed_seed(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void test_relative_altitude_math(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 1U,
        true, true, true, 10000));
    assert(nrf_height_reference_relative_z_mm(&reference, true, 12500) == 2500);
    assert(nrf_height_reference_relative_z_mm(&reference, true, 7500) == -2500);
    assert(nrf_height_reference_relative_z_mm(&reference, false, 12500) == 0);
}

static void test_capture_requires_valid_stationary_sources(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 3U,
        false, true, true, 9000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    /* Every outcome answers the request it belongs to: the ESP matches reference_generation to its request. */
    assert(reference.generation == 3U);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 4U,
        true, false, true, 9000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(reference.generation == 4U);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 5U,
        true, true, false, 9000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    assert(reference.generation == 5U);
    assert(reference.altitude_mm == 0);
}

static void test_capture_clear_and_failure_state_semantics(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(reference.state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 10U,
        true, true, true, 12345));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 11U,
        true, true, false, 13000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    assert(reference.generation == 11U);
    assert(nrf_height_reference_relative_z_mm(&reference, true, 13000) == 0);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR, 12U,
        false, false, false, 0));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(reference.generation == 12U && reference.altitude_mm == 0);
    assert(!nrf_height_reference_apply(&reference, 99U, 13U, true, true, true, 0));
}

static void test_altitude_noise_and_drift_keep_absolute_value(void)
{
    nrf_height_reference_t reference;
    int32_t absolute_altitude = 25000;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 20U,
        true, true, true, absolute_altitude));
    for (int32_t drift = -2000; drift <= 2000; drift += 250) {
        const int32_t noisy_altitude = absolute_altitude + drift + 25;
        assert(nrf_height_reference_relative_z_mm(
                   &reference, true, noisy_altitude) == drift + 25);
        assert(absolute_altitude == 25000);
    }
}

static void test_absolute_altitude_is_not_modified(void)
{
    nrf_height_reference_t reference;
    const int32_t absolute_altitude = 41875;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 21U,
        true, true, true, 40000));
    assert(nrf_height_reference_relative_z_mm(
               &reference, true, absolute_altitude) == 1875);
    assert(absolute_altitude == 41875);
}

static void test_new_lifecycle_starts_without_reference(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 22U,
        true, true, true, 1234));
    nrf_height_reference_init(&reference);
    assert(reference.state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(reference.generation == 0U && reference.altitude_mm == 0);
}

static void test_generation_and_reference_provenance(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 0xABCDU,
        true, true, true, 7000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(reference.generation == 0xABCDU);
}

static void test_reference_instances_are_independent(void)
{
    nrf_height_reference_t nodes[4];
    for (size_t i = 0U; i < 4U; ++i) {
        nrf_height_reference_init(&nodes[i]);
    }
    assert(nrf_height_reference_apply(
        &nodes[0], NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 1U,
        true, true, true, 1000));
    assert(nrf_height_reference_apply(
        &nodes[1], NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 2U,
        true, true, true, 3000));
    assert(nrf_height_reference_apply(
        &nodes[2], NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 3U,
        true, true, false, 5000));
    assert(nodes[0].state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(nodes[1].state == NRF_UPSTREAM_REFERENCE_VALID);
    assert(nodes[2].state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    assert(nodes[3].state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(nrf_height_reference_relative_z_mm(&nodes[0], true, 1500) == 500);
    assert(nrf_height_reference_relative_z_mm(&nodes[1], true, 1500) == -1500);
}

static void test_fixed_seed_relative_altitude_properties(void)
{
    nrf_height_reference_t reference;
    uint32_t seed = 0x6D2B79F5U;
    const int32_t min_altitude = -500000;
    const uint32_t altitude_span = 6500001U;
    const int32_t reference_altitude = min_altitude +
        (int32_t)(next_fixed_seed(&seed) % altitude_span);
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 0x100U,
        true, true, true, reference_altitude));
    for (unsigned i = 0U; i < 1000U; ++i) {
        const int32_t current = min_altitude +
            (int32_t)(next_fixed_seed(&seed) % altitude_span);
        assert(nrf_height_reference_relative_z_mm(&reference, true, current) ==
               current - reference_altitude);
    }
}

static void test_sample_holds_last_good_z_through_a_barometer_outage_and_restores(void)
{
    nrf_height_reference_t reference;
    int32_t z = -1;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 30U, true, true, true, 10000));

    assert(nrf_height_reference_sample(&reference, true, 11500, 1000U, &z) ==
           NRF_UPSTREAM_REFERENCE_VALID);
    assert(z == 1500);

    /* Outage inside the hold window: the generation stays and Z is the last good value. */
    assert(nrf_height_reference_sample(&reference, false, 0, 1000U + 5000U, &z) ==
           NRF_UPSTREAM_REFERENCE_VALID);
    assert(z == 1500 && reference.generation == 30U);
    assert(nrf_height_reference_sample(
               &reference, false, 0, 1000U + NRF_HEIGHT_REFERENCE_HOLD_MS, &z) ==
           NRF_UPSTREAM_REFERENCE_VALID);
    assert(z == 1500);

    /* Past the hold window Z is voided, but the stored reference survives. */
    assert(nrf_height_reference_sample(
               &reference, false, 0, 1000U + NRF_HEIGHT_REFERENCE_HOLD_MS + 1U, &z) ==
           NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(z == 0 && reference.state == NRF_UPSTREAM_REFERENCE_VALID);

    /* A fresh sample restores it against the original reference altitude. */
    assert(nrf_height_reference_sample(
               &reference, true, 10800, 1000U + NRF_HEIGHT_REFERENCE_HOLD_MS + 2000U, &z) ==
           NRF_UPSTREAM_REFERENCE_VALID);
    assert(z == 800 && reference.generation == 30U);
}

static void test_sample_without_any_live_sample_holds_the_capture_point(void)
{
    nrf_height_reference_t reference;
    int32_t z = -1;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 31U, true, true, true, 5000));
    /* Barometer lost right after the capture: Z = 0 is the truthful held value. */
    assert(nrf_height_reference_sample(&reference, false, 0, 777U, &z) ==
           NRF_UPSTREAM_REFERENCE_VALID);
    assert(z == 0);
    assert(nrf_height_reference_sample(
               &reference, false, 0, 777U + NRF_HEIGHT_REFERENCE_HOLD_MS + 1U, &z) ==
           NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
}

static void test_sample_never_invents_a_reference(void)
{
    nrf_height_reference_t reference;
    int32_t z = -1;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_sample(&reference, true, 9000, 10U, &z) ==
           NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(z == 0);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 32U, true, false, true, 9000));
    assert(nrf_height_reference_sample(&reference, true, 9000, 20U, &z) ==
           NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(z == 0);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR, 33U, true, true, true, 0));
    assert(nrf_height_reference_sample(&reference, false, 0, 30U, &z) ==
           NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(nrf_height_reference_sample(NULL, true, 0, 0U, &z) ==
           NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
}

static void test_window_centre_needs_three_samples_else_the_fallback(void)
{
    const int32_t two[2] = { 1000, 1010 };
    assert(nrf_height_reference_window_centre_mm(two, 2U, 777) == 777);
    assert(nrf_height_reference_window_centre_mm(NULL, 5U, 777) == 777);
    assert(nrf_height_reference_window_centre_mm(two, 0U, 777) == 777);
}

static void test_window_centre_is_the_mean_of_quiet_samples(void)
{
    const int32_t quiet[5] = { 1000, 1010, 990, 1005, 995 };
    assert(nrf_height_reference_window_centre_mm(quiet, 5U, 0) == 1000);
    const int32_t steady[5] = { -250, -250, -250, -250, -250 };
    assert(nrf_height_reference_window_centre_mm(steady, 5U, 0) == -250);
    const int32_t three[3] = { 100, 160, 130 };
    assert(nrf_height_reference_window_centre_mm(three, 3U, 0) == 130);
    /* Half-millimetres round away from zero. */
    const int32_t halves[4] = { 0, 1, 0, 1 };
    assert(nrf_height_reference_window_centre_mm(halves, 4U, 99) == 1);
    const int32_t negative_halves[4] = { 0, -1, 0, -1 };
    assert(nrf_height_reference_window_centre_mm(negative_halves, 4U, 99) == -1);
}

static void test_window_centre_drops_one_outlier_but_not_two(void)
{
    /* One reading 900 mm away from four steady ones is a glitch: the centre is the steady value. */
    const int32_t one[5] = { 2000, 2010, 2900, 1990, 2000 };
    assert(nrf_height_reference_window_centre_mm(one, 5U, 0) == 2000);
    /* Two of five is not a glitch: nothing is dropped, the mean of all five is the centre. */
    const int32_t two[5] = { 2000, 2900, 2010, 2900, 1990 };
    assert(nrf_height_reference_window_centre_mm(two, 5U, 0) == 2360);
    /* A spread no wider than three sigma_r is ordinary noise (the floor is 40 mm): nothing is an outlier. */
    const int32_t noisy[5] = { 2000, 2100, 1900, 2110, 1890 };
    assert(nrf_height_reference_window_centre_mm(noisy, 5U, 0) == 2000);
}

static void test_window_centre_uses_only_the_newest_samples(void)
{
    /* Seven values were given; the window is the last five. */
    const int32_t many[7] = { 9000, 9000, 1000, 1010, 990, 1005, 995 };
    assert(nrf_height_reference_window_centre_mm(many, 7U, 0) == 1000);
}

/*
 * A window is one position when no more samples lie beyond 3 sigma_r of the median than the centre may drop (one of
 * five). Barometer noise (BMP280 x16 with IIR 16: about 0.2 Pa RMS, 2 cm) and a single glitch are within that; a node
 * moved by one metre (12 Pa) leaves two or more samples of its old place 25 sigma_r away.
 */
#if !defined(OLD_API) /* OLD_API: build against the previous module, which has neither function */
static void test_window_settled_tells_noise_from_a_move(void)
{
    static const int32_t quiet[5] = {10000, 10030, 9980, 10010, 9990};
    static const int32_t one_glitch[5] = {10000, 10030, 11000, 10010, 9990};
    static const int32_t moved_two_ago[5] = {10000, 10010, 9990, 11000, 11010};
    static const int32_t moved_three_ago[5] = {10000, 10010, 11000, 11010, 10990};
    static const int32_t fresh_after_move[3] = {11000, 11010, 10990};
    assert(nrf_height_reference_window_settled(quiet, 5U));
    assert(nrf_height_reference_window_settled(one_glitch, 5U));
    assert(!nrf_height_reference_window_settled(moved_two_ago, 5U));
    assert(!nrf_height_reference_window_settled(moved_three_ago, 5U));
    assert(nrf_height_reference_window_settled(fresh_after_move, 3U));
    assert(!nrf_height_reference_window_settled(fresh_after_move, 2U));
    assert(!nrf_height_reference_window_settled(NULL, 5U));
}

/*
 * A move the gyro does not see (lifted straight up) shows in the barometer alone: one new altitude is a suspect (glitch
 * or move), two consistent ones of a new level are a step. Noise and a glitch that went back are neither.
 */
static void test_window_tells_a_step_from_a_glitch(void)
{
    static const int32_t quiet[5] = {10000, 10030, 9980, 10010, 9990};
    static const int32_t newest_up[5] = {10000, 10030, 9980, 10010, 11000};
    static const int32_t glitch_back[5] = {10030, 9980, 10010, 11000, 10000};
    static const int32_t step_two[5] = {10030, 9980, 10010, 11000, 11010};
    static const int32_t step_down[4] = {10000, 10010, 9000, 9020};
    static const int32_t diverging[5] = {10030, 9980, 10010, 11000, 12000};
    assert(!nrf_height_reference_window_newest_is_outlier(quiet, 5U));
    assert(nrf_height_reference_window_newest_is_outlier(newest_up, 5U));
    assert(!nrf_height_reference_window_newest_is_outlier(glitch_back, 5U));
    assert(!nrf_height_reference_window_step(quiet, 5U));
    assert(!nrf_height_reference_window_step(newest_up, 5U));
    assert(!nrf_height_reference_window_step(glitch_back, 5U));
    assert(nrf_height_reference_window_step(step_two, 5U));
    assert(nrf_height_reference_window_step(step_down, 4U));
    assert(!nrf_height_reference_window_step(diverging, 5U));
    assert(!nrf_height_reference_window_step(step_two, 2U));
}

static void test_pending_capture_answers_under_the_requested_generation(void)
{
    nrf_height_reference_t reference;
    nrf_height_reference_init(&reference);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 20U, true, true, true, 5000));
    nrf_height_reference_begin_pending(&reference, 21U);
    assert(reference.state == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(reference.generation == 21U && reference.altitude_mm == 0);
    int32_t z = 7;
    assert(nrf_height_reference_sample(&reference, true, 6000, 1000U, &z) == NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED);
    assert(z == 0);
}
#endif

int main(void)
{
#if !defined(OLD_API)
    test_window_settled_tells_noise_from_a_move();
    test_window_tells_a_step_from_a_glitch();
    test_pending_capture_answers_under_the_requested_generation();
#endif
    test_relative_altitude_math();
    test_capture_requires_valid_stationary_sources();
    test_capture_clear_and_failure_state_semantics();
    test_altitude_noise_and_drift_keep_absolute_value();
    test_absolute_altitude_is_not_modified();
    test_new_lifecycle_starts_without_reference();
    test_generation_and_reference_provenance();
    test_reference_instances_are_independent();
    test_fixed_seed_relative_altitude_properties();
    test_sample_holds_last_good_z_through_a_barometer_outage_and_restores();
    test_sample_without_any_live_sample_holds_the_capture_point();
    test_sample_never_invents_a_reference();
    test_window_centre_needs_three_samples_else_the_fallback();
    test_window_centre_is_the_mean_of_quiet_samples();
    test_window_centre_drops_one_outlier_but_not_two();
    test_window_centre_uses_only_the_newest_samples();
    return 0;
}
