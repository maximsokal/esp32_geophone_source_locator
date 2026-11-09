#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nrf_ema_filter.h"
#include "nrf_orientation.h"

static void test_timestamp_alpha_and_seed(void)
{
    nrf_ema_filter_t filter;
    double value = -1.0;

    assert(nrf_ema_filter_init(&filter, 100U, -20.0, 20.0));
    assert(nrf_ema_filter_update(&filter, 0.0, 100U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    assert(value == 0.0);
    assert(nrf_ema_filter_update(&filter, 10.0, 200U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    /* dt = tau: the exact step response 10 (1 - e^-1). */
    assert(fabs(value - 10.0 * (1.0 - exp(-1.0))) < 1.0e-12);
}

/*
 * A first-order low-pass y' = (x - y) / tau driven by a step of height A
 * from y = 0 has the closed-form response y(t) = A (1 - e^(-t/tau))
 * (time constant: 63.2 % at t = tau). Its exact zero-order-hold
 * discretisation, G(z) = (1 - e^(-T/tau)) / (z - e^(-T/tau)), is the
 * recursion y += (1 - e^(-dt/tau)) (x - y). The filter samples it: whatever the spacing of the samples, regular or
 * not, the value at t must be that response, and two half steps must equal
 * one whole step. An implicit-Euler alpha = dt / (tau + dt) gives 5.0 for
 * dt = tau instead of 6.32 and depends on the sample spacing.
 */
static double step_response_at(const uint64_t *offsets_ms, size_t count, uint32_t tau_ms)
{
    nrf_ema_filter_t filter;
    double value = 0.0;
    assert(nrf_ema_filter_init(&filter, tau_ms, -100.0, 100.0));
    assert(nrf_ema_filter_update(&filter, 0.0, 1000U, &value) == NRF_EMA_FILTER_UPDATED);
    for (size_t i = 0U; i < count; ++i) {
        assert(nrf_ema_filter_update(&filter, 10.0, 1000U + offsets_ms[i], &value) ==
               NRF_EMA_FILTER_UPDATED);
    }
    return value;
}

static void test_step_response_is_the_exponential_whatever_the_sample_spacing(void)
{
    const uint32_t tau_ms = 1000U;
    /* MAIN barometer: 200 ms polls; REMOTE: 2 s refreshes; a late, irregular poll sequence. */
    static const uint64_t main_polls[] = { 200U, 400U, 600U, 800U, 1000U, 1200U, 1400U, 1600U, 1800U, 2000U };
    static const uint64_t remote_refresh[] = { 2000U };
    static const uint64_t irregular[] = { 130U, 610U, 700U, 1490U, 2000U };
    const double expected = 10.0 * (1.0 - exp(-2000.0 / (double)tau_ms));

    const double main_value = step_response_at(main_polls, sizeof(main_polls) / sizeof(main_polls[0]), tau_ms);
    const double remote_value = step_response_at(remote_refresh, 1U, tau_ms);
    const double irregular_value = step_response_at(irregular, sizeof(irregular) / sizeof(irregular[0]), tau_ms);
    printf("[EMA] y(2 tau): exact %.9f main %.9f remote %.9f irregular %.9f\n",
           expected, main_value, remote_value, irregular_value);
    assert(fabs(main_value - expected) < 1.0e-9);
    assert(fabs(remote_value - expected) < 1.0e-9);
    assert(fabs(irregular_value - expected) < 1.0e-9);
}

static void test_invalid_values_and_timestamps_do_not_change_state(void)
{
    nrf_ema_filter_t filter;
    double value = 55.0;
    double before;
    uint64_t before_timestamp;

    assert(nrf_ema_filter_init(&filter, 200U, -10.0, 10.0));
    assert(nrf_ema_filter_update(&filter, 2.0, 50U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    before = filter.value;
    before_timestamp = filter.timestamp_ms;
    value = 55.0;
    assert(nrf_ema_filter_update(&filter, NAN, 60U, &value) ==
           NRF_EMA_FILTER_INVALID_SAMPLE);
    assert(nrf_ema_filter_update(&filter, INFINITY, 60U, &value) ==
           NRF_EMA_FILTER_INVALID_SAMPLE);
    assert(nrf_ema_filter_update(&filter, 11.0, 60U, &value) ==
           NRF_EMA_FILTER_INVALID_SAMPLE);
    assert(nrf_ema_filter_update(&filter, 3.0, 50U, &value) ==
           NRF_EMA_FILTER_INVALID_TIME);
    assert(nrf_ema_filter_update(&filter, 3.0, 49U, &value) ==
           NRF_EMA_FILTER_INVALID_TIME);
    assert(filter.value == before && filter.timestamp_ms == before_timestamp);
    assert(value == 55.0);
}

static void test_noise_attenuation_and_gap_reseed(void)
{
    nrf_ema_filter_t filter;
    double value;

    assert(nrf_ema_filter_init(&filter, 200U, -100.0, 100.0));
    assert(nrf_ema_filter_update(&filter, 0.0, 0U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    for (uint64_t t = 100U; t <= 2000U; t += 100U) {
        assert(nrf_ema_filter_update(
                   &filter, (t / 100U) % 2U == 0U ? 10.0 : -10.0,
                   t, &value) == NRF_EMA_FILTER_UPDATED);
        assert(value > -10.0 && value < 10.0);
    }
    assert(nrf_ema_filter_update(&filter, 40.0, 3001U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    assert(value == 40.0);
}

static void test_fault_reset_and_recovery(void)
{
    nrf_ema_filter_t filter;
    double value;

    assert(nrf_ema_filter_init(&filter, 100U, -50.0, 50.0));
    assert(nrf_ema_filter_update(&filter, 10.0, 100U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    nrf_ema_filter_reset(&filter);
    assert(!filter.initialized);
    assert(nrf_ema_filter_update(&filter, -5.0, 1U, &value) ==
           NRF_EMA_FILTER_UPDATED);
    assert(value == -5.0);
}

static void test_bounded_output_and_instance_isolation(void)
{
    nrf_ema_filter_t slow;
    nrf_ema_filter_t fast;
    double slow_value;
    double fast_value;

    assert(nrf_ema_filter_init(&slow, 1000U, -100.0, 100.0));
    assert(nrf_ema_filter_init(&fast, 100U, -100.0, 100.0));
    assert(nrf_ema_filter_update(&slow, 0.0, 10U, &slow_value) ==
           NRF_EMA_FILTER_UPDATED);
    assert(nrf_ema_filter_update(&fast, 0.0, 10U, &fast_value) ==
           NRF_EMA_FILTER_UPDATED);
    for (int sample = -100; sample <= 100; sample += 5) {
        const uint64_t timestamp = (uint64_t)(sample + 101) * 10U + 10U;
        assert(nrf_ema_filter_update(
                   &slow, (double)sample, timestamp, &slow_value) ==
               NRF_EMA_FILTER_UPDATED);
        assert(nrf_ema_filter_update(
                   &fast, (double)sample, timestamp, &fast_value) ==
               NRF_EMA_FILTER_UPDATED);
        assert(slow_value >= -100.0 && slow_value <= 100.0);
        assert(fast_value >= -100.0 && fast_value <= 100.0);
    }
    assert(slow_value != fast_value);
    assert(slow.timestamp_ms == fast.timestamp_ms);
}

static void test_reference_trace_orientation_regression(void)
{
    nrf_ema_filter_t filters[3];
    nrf_orientation_config_t old_config;
    nrf_orientation_config_t filtered_config;
    nrf_orientation_estimator_t old_estimator;
    nrf_orientation_estimator_t filtered_estimator;
    unsigned old_errors = 0U;
    unsigned filtered_errors = 0U;
    unsigned accepted_count = 0U;
    unsigned tilted_count = 0U;

    for (size_t axis = 0U; axis < 3U; ++axis) {
        assert(nrf_ema_filter_init(&filters[axis], 200U, -2000.0, 2000.0));
    }
    nrf_orientation_default_config(&old_config);
    old_config.reference_axis = NRF_ORIENTATION_AXIS_POS_Z;
    filtered_config = old_config;
    filtered_config.ema_shift = 0U;
    assert(nrf_orientation_estimator_init(&old_estimator, &old_config));
    assert(nrf_orientation_estimator_init(&filtered_estimator, &filtered_config));

    for (unsigned index = 0U; index < 120U; ++index) {
        nrf_imu_sample_t raw = {0};
        nrf_imu_sample_t filtered = {0};
        bool labeled;
        bool expected_accepted;
        const uint64_t timestamp = 1000U + (uint64_t)index * 100U;

        if (index < 40U || index >= 80U) {
            raw.accel_mg[2] = 1000;
        } else {
            raw.accel_mg[0] = 500;
            raw.accel_mg[2] = 866;
        }
        for (size_t axis = 0U; axis < 3U; ++axis) {
            double value;
            assert(nrf_ema_filter_update(
                       &filters[axis], (double)raw.accel_mg[axis],
                       timestamp, &value) == NRF_EMA_FILTER_UPDATED);
            filtered.accel_mg[axis] = (int32_t)(
                value >= 0.0 ? value + 0.5 : -(-value + 0.5));
        }
        assert(nrf_orientation_estimator_update(&old_estimator, &raw, true));
        assert(nrf_orientation_estimator_update(&filtered_estimator, &filtered, true));

        labeled = (index >= 20U && index < 40U) ||
                  (index >= 60U && index < 80U) ||
                  (index >= 100U);
        if (!labeled) {
            continue;
        }
        expected_accepted = index < 40U || index >= 80U;
        const bool old_accepted =
            old_estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED;
        const bool filtered_accepted =
            filtered_estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED;
        old_errors += old_accepted != expected_accepted;
        filtered_errors += filtered_accepted != expected_accepted;
        if (filtered_accepted) {
            ++accepted_count;
        } else if (filtered_estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE) {
            ++tilted_count;
        }
    }
    assert(filtered_errors <= old_errors);
    assert(accepted_count > 0U && tilted_count > 0U);
}

int main(void)
{
    nrf_ema_filter_t invalid_config;
    assert(!nrf_ema_filter_init(&invalid_config, 0U, 0.0, 1.0));
    assert(!nrf_ema_filter_init(&invalid_config, 1U, 2.0, 1.0));
    test_step_response_is_the_exponential_whatever_the_sample_spacing();
    test_timestamp_alpha_and_seed();
    test_invalid_values_and_timestamps_do_not_change_state();
    test_noise_attenuation_and_gap_reseed();
    test_fault_reset_and_recovery();
    test_bounded_output_and_instance_isolation();
    test_reference_trace_orientation_regression();
    return 0;
}
