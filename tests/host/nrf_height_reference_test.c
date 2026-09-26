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
    assert(reference.generation == 0U);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 4U,
        true, false, true, 9000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_SENSOR_INVALID);
    assert(reference.generation == 0U);
    assert(nrf_height_reference_apply(
        &reference, NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE, 5U,
        true, true, false, 9000));
    assert(reference.state == NRF_UPSTREAM_REFERENCE_REJECTED_MOVING);
    assert(reference.generation == 0U);
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
    assert(reference.generation == 10U);
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

int main(void)
{
    test_relative_altitude_math();
    test_capture_requires_valid_stationary_sources();
    test_capture_clear_and_failure_state_semantics();
    test_altitude_noise_and_drift_keep_absolute_value();
    test_absolute_altitude_is_not_modified();
    test_new_lifecycle_starts_without_reference();
    test_generation_and_reference_provenance();
    test_reference_instances_are_independent();
    test_fixed_seed_relative_altitude_properties();
    return 0;
}
