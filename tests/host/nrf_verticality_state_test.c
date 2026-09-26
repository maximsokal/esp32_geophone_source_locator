#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_blue_led.h"
#include "nrf_verticality_state.h"

static nrf_verticality_estimator_t make_estimator(void)
{
    nrf_verticality_config_t config;
    nrf_verticality_estimator_t estimator;
    nrf_verticality_default_config(&config);
    assert(nrf_verticality_state_init(&estimator, &config));
    return estimator;
}

static nrf_verticality_state_t sample(
    nrf_verticality_estimator_t *estimator,
    uint64_t time_ms,
    uint32_t tilt_sin2_ppm,
    uint32_t gyro_mdps)
{
    return nrf_verticality_state_update(
        estimator, true, true, tilt_sin2_ppm, gyro_mdps, time_ms);
}

static void accept_at_default_dwell(nrf_verticality_estimator_t *estimator)
{
    assert(sample(estimator, 0U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    for (uint64_t time_ms = 100U; time_ms < 750U; time_ms += 100U) {
        assert(sample(estimator, time_ms, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    }
    assert(sample(estimator, 750U, 0U, 0U) == NRF_VERTICALITY_ACCEPTED);
}

static void test_initial_state_is_fault(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    assert(estimator.state == NRF_VERTICALITY_FAULT);
}

static void test_dwell_requires_configured_continuous_stability(void)
{
    nrf_verticality_config_t config;
    nrf_verticality_estimator_t estimator;
    nrf_verticality_default_config(&config);
    config.dwell_ms = 1000U;
    assert(nrf_verticality_state_init(&estimator, &config));
    assert(sample(&estimator, 100U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 350U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 600U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 850U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 1099U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 1100U, 0U, 0U) == NRF_VERTICALITY_ACCEPTED);
}

static void test_tilt_hysteresis_and_boundaries(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    accept_at_default_dwell(&estimator);
    assert(sample(&estimator, 850U, 25000U, 0U) == NRF_VERTICALITY_ACCEPTED);
    assert(sample(&estimator, 950U, 30154U, 0U) == NRF_VERTICALITY_TILTED);
    assert(sample(&estimator, 1050U, 19369U, 0U) == NRF_VERTICALITY_STABILIZING);
}

static void test_sustained_motion_exit_bound(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    accept_at_default_dwell(&estimator);
    assert(sample(&estimator, 800U, 0U, 15000U) == NRF_VERTICALITY_ACCEPTED);
    assert(sample(&estimator, 999U, 0U, 15000U) == NRF_VERTICALITY_ACCEPTED);
    assert(sample(&estimator, 1000U, 0U, 15000U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 1100U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    for (uint64_t time_ms = 1200U; time_ms < 1850U; time_ms += 100U) {
        assert(sample(&estimator, time_ms, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    }
    assert(sample(&estimator, 1850U, 0U, 0U) == NRF_VERTICALITY_ACCEPTED);
}

static void test_invalid_imu_sets_fault_and_recovery_needs_dwell(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    assert(nrf_verticality_state_update(
               &estimator, false, false, 0U, 0U, 0U) == NRF_VERTICALITY_FAULT);
    assert(sample(&estimator, 100U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    for (uint64_t time_ms = 200U; time_ms <= 800U; time_ms += 100U) {
        assert(sample(&estimator, time_ms, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    }
    assert(sample(&estimator, 849U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 850U, 0U, 0U) == NRF_VERTICALITY_ACCEPTED);
}

static void test_unusable_tilt_sets_fault(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    assert(nrf_verticality_state_update(
               &estimator, true, false, 0U, 0U, 0U) == NRF_VERTICALITY_FAULT);
    assert(nrf_verticality_state_update(
               &estimator, true, true, 1000001U, 0U, 100U) ==
           NRF_VERTICALITY_FAULT);
}

static void test_sample_gap_restarts_dwell(void)
{
    nrf_verticality_estimator_t estimator = make_estimator();
    assert(sample(&estimator, 0U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 300U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    assert(sample(&estimator, 601U, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    for (uint64_t time_ms = 701U; time_ms < 1351U; time_ms += 100U) {
        assert(sample(&estimator, time_ms, 0U, 0U) == NRF_VERTICALITY_STABILIZING);
    }
    assert(sample(&estimator, 1351U, 0U, 0U) == NRF_VERTICALITY_ACCEPTED);
}

static void test_four_instances_are_independent(void)
{
    nrf_verticality_estimator_t nodes[4];
    for (size_t index = 0U; index < 4U; ++index) {
        nodes[index] = make_estimator();
    }
    accept_at_default_dwell(&nodes[0]);
    assert(sample(&nodes[1], 0U, 50000U, 0U) == NRF_VERTICALITY_TILTED);
    assert(sample(&nodes[2], 0U, 0U, 20000U) == NRF_VERTICALITY_STABILIZING);
    assert(nrf_verticality_state_update(
               &nodes[3], false, false, 0U, 0U, 0U) == NRF_VERTICALITY_FAULT);
    assert(nodes[0].state == NRF_VERTICALITY_ACCEPTED);
    assert(nodes[1].state == NRF_VERTICALITY_TILTED);
    assert(nodes[2].state == NRF_VERTICALITY_STABILIZING);
    assert(nodes[3].state == NRF_VERTICALITY_FAULT);
}

static void test_wire_state_and_led_policy_share_verticality_state(void)
{
    assert(nrf_verticality_state_to_orientation_wire(NRF_VERTICALITY_FAULT) ==
           (uint8_t)NRF_ORIENTATION_STATE_UNAVAILABLE);
    assert(nrf_blue_led_policy(NRF_VERTICALITY_FAULT) == NRF_BLUE_LED_OFF);
    assert(nrf_verticality_state_to_orientation_wire(NRF_VERTICALITY_STABILIZING) ==
           (uint8_t)NRF_ORIENTATION_STATE_MOVING);
    assert(nrf_blue_led_policy(NRF_VERTICALITY_STABILIZING) ==
           NRF_BLUE_LED_BLINK_2HZ);
    assert(nrf_verticality_state_to_orientation_wire(NRF_VERTICALITY_ACCEPTED) ==
           (uint8_t)NRF_ORIENTATION_STATE_ACCEPTED);
    assert(nrf_blue_led_policy(NRF_VERTICALITY_ACCEPTED) == NRF_BLUE_LED_ON);
    assert(nrf_verticality_state_to_orientation_wire(NRF_VERTICALITY_TILTED) ==
           (uint8_t)NRF_ORIENTATION_STATE_OUTSIDE);
    assert(nrf_blue_led_policy(NRF_VERTICALITY_TILTED) == NRF_BLUE_LED_BLINK_2HZ);
    assert(NRF_VERTICALITY_DEFAULT_DWELL_MS == 750U);
    assert(NRF_VERTICALITY_DEFAULT_MOTION_EXIT_MS == 200U);
}

int main(void)
{
    nrf_verticality_config_t invalid;
    nrf_verticality_default_config(&invalid);
    assert(nrf_verticality_config_valid(&invalid));
    invalid.dwell_ms = 0U;
    assert(!nrf_verticality_config_valid(&invalid));
    test_initial_state_is_fault();
    test_dwell_requires_configured_continuous_stability();
    test_tilt_hysteresis_and_boundaries();
    test_sustained_motion_exit_bound();
    test_invalid_imu_sets_fault_and_recovery_needs_dwell();
    test_unusable_tilt_sets_fault();
    test_sample_gap_restarts_dwell();
    test_four_instances_are_independent();
    test_wire_state_and_led_policy_share_verticality_state();
    return 0;
}
