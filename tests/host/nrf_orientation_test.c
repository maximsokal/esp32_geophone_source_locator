#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_orientation.h"

static nrf_orientation_config_t config_for(nrf_orientation_reference_axis_t axis)
{
    nrf_orientation_config_t config;
    nrf_orientation_default_config(&config);
    config.reference_axis = axis;
    config.ema_shift = 0U;
    return config;
}

static nrf_imu_sample_t sample_xyz(
    int32_t x,
    int32_t y,
    int32_t z,
    int32_t gx,
    int32_t gy,
    int32_t gz)
{
    nrf_imu_sample_t sample;
    memset(&sample, 0, sizeof(sample));
    sample.accel_mg[0] = x;
    sample.accel_mg[1] = y;
    sample.accel_mg[2] = z;
    sample.gyro_mdps[0] = gx;
    sample.gyro_mdps[1] = gy;
    sample.gyro_mdps[2] = gz;
    return sample;
}

static void assert_axis_accepts(
    nrf_orientation_reference_axis_t axis,
    int32_t x,
    int32_t y,
    int32_t z)
{
    nrf_orientation_estimator_t estimator;
    nrf_orientation_config_t config = config_for(axis);
    nrf_imu_sample_t sample = sample_xyz(x, y, z, 0, 0, 0);
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &sample, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);
}

static void test_every_signed_axis(void)
{
    assert_axis_accepts(NRF_ORIENTATION_AXIS_POS_X, 1000, 0, 0);
    assert_axis_accepts(NRF_ORIENTATION_AXIS_NEG_X, -1000, 0, 0);
    assert_axis_accepts(NRF_ORIENTATION_AXIS_POS_Y, 0, 1000, 0);
    assert_axis_accepts(NRF_ORIENTATION_AXIS_NEG_Y, 0, -1000, 0);
    assert_axis_accepts(NRF_ORIENTATION_AXIS_POS_Z, 0, 0, 1000);
    assert_axis_accepts(NRF_ORIENTATION_AXIS_NEG_Z, 0, 0, -1000);
}

static void test_unconfigured_and_invalid_sample(void)
{
    nrf_orientation_config_t config;
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t sample = sample_xyz(0, 0, 1000, 0, 0, 0);
    nrf_orientation_default_config(&config);
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(!nrf_orientation_estimator_update(&estimator, &sample, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_UNAVAILABLE);
    assert(estimator.output.reason == NRF_ORIENTATION_REASON_UNCONFIGURED);

    config.reference_axis = NRF_ORIENTATION_AXIS_POS_Z;
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(!nrf_orientation_estimator_update(&estimator, &sample, false));
    assert(!estimator.output.sample_valid);
    assert(estimator.output.reason == NRF_ORIENTATION_REASON_SAMPLE_INVALID);
}

static void test_hysteresis_and_boundary(void)
{
    nrf_orientation_config_t config = config_for(NRF_ORIENTATION_AXIS_POS_Z);
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t vertical = sample_xyz(0, 0, 1000, 0, 0, 0);
    nrf_imu_sample_t about_nine_deg = sample_xyz(156, 0, 988, 0, 0, 0);
    nrf_imu_sample_t about_eleven_deg = sample_xyz(191, 0, 982, 0, 0, 0);

    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &vertical, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);

    assert(nrf_orientation_estimator_update(&estimator, &about_nine_deg, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);

    assert(nrf_orientation_estimator_update(&estimator, &about_eleven_deg, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE);

    assert(nrf_orientation_estimator_update(&estimator, &about_nine_deg, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE);

    assert(nrf_orientation_estimator_update(&estimator, &vertical, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);
}


static void test_exact_threshold_boundary(void)
{
    nrf_orientation_config_t config = config_for(NRF_ORIENTATION_AXIS_POS_Z);
    nrf_orientation_estimator_t probe;
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t sample = sample_xyz(200, 0, 980, 0, 0, 0);
    uint32_t metric;

    assert(nrf_orientation_estimator_init(&probe, &config));
    assert(nrf_orientation_estimator_update(&probe, &sample, true));
    metric = probe.output.tilt_sin2_ppm;
    assert(metric > 0U);

    config.accept_sin2_ppm = metric;
    config.reenter_sin2_ppm = metric;
    config.medium_sin2_ppm = metric + 1U;
    config.fast_sin2_ppm = metric + 2U;
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &sample, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);

    config.accept_sin2_ppm = metric - 1U;
    config.reenter_sin2_ppm = metric - 1U;
    config.medium_sin2_ppm = metric;
    config.fast_sin2_ppm = metric + 1U;
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &sample, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE);
}

static void test_wrong_direction_and_severity(void)
{
    nrf_orientation_config_t config = config_for(NRF_ORIENTATION_AXIS_POS_Z);
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t wrong = sample_xyz(0, 0, -1000, 0, 0, 0);
    nrf_imu_sample_t near_vertical = sample_xyz(259, 0, 966, 0, 0, 0);  /* ~15 deg */
    nrf_imu_sample_t mid_tilt = sample_xyz(423, 0, 906, 0, 0, 0);       /* ~25 deg */
    nrf_imu_sample_t far_tilt = sample_xyz(643, 0, 766, 0, 0, 0);       /* ~40 deg */

    /* Blink rate is a "getting warmer" signal: closer to the accepted
     * vertical band blinks FASTER, a large/wrong-direction tilt blinks
     * SLOWEST, and ACCEPTED (not exercised here) is steady. */
    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &wrong, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE);
    assert(estimator.output.blink_class == NRF_ORIENTATION_BLINK_SLOW);

    assert(nrf_orientation_estimator_update(&estimator, &near_vertical, true));
    assert(estimator.output.blink_class == NRF_ORIENTATION_BLINK_FAST);
    assert(nrf_orientation_estimator_update(&estimator, &mid_tilt, true));
    assert(estimator.output.blink_class == NRF_ORIENTATION_BLINK_MEDIUM);
    assert(nrf_orientation_estimator_update(&estimator, &far_tilt, true));
    assert(estimator.output.blink_class == NRF_ORIENTATION_BLINK_SLOW);
}

static void test_gravity_and_motion_gates(void)
{
    nrf_orientation_config_t config = config_for(NRF_ORIENTATION_AXIS_POS_Z);
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t low_g = sample_xyz(0, 0, 700, 0, 0, 0);
    nrf_imu_sample_t high_g = sample_xyz(0, 0, 1300, 0, 0, 0);
    nrf_imu_sample_t moving = sample_xyz(0, 0, 1000, 16000, 0, 0);

    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(!nrf_orientation_estimator_update(&estimator, &low_g, true));
    assert(estimator.output.reason == NRF_ORIENTATION_REASON_GRAVITY_RANGE);
    assert(!nrf_orientation_estimator_update(&estimator, &high_g, true));
    assert(estimator.output.reason == NRF_ORIENTATION_REASON_GRAVITY_RANGE);
    assert(nrf_orientation_estimator_update(&estimator, &moving, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_MOVING);
    assert(estimator.output.reason == NRF_ORIENTATION_REASON_MOTION);
    assert(!estimator.output.sample_valid);
}

static void test_filter_convergence_and_recovery(void)
{
    nrf_orientation_config_t config = config_for(NRF_ORIENTATION_AXIS_POS_Z);
    nrf_orientation_estimator_t estimator;
    nrf_imu_sample_t vertical = sample_xyz(0, 0, 1000, 0, 0, 0);
    nrf_imu_sample_t tilted = sample_xyz(500, 0, 866, 0, 0, 0);
    nrf_imu_sample_t moving = sample_xyz(500, 0, 866, 20000, 0, 0);
    config.ema_shift = 2U;

    assert(nrf_orientation_estimator_init(&estimator, &config));
    assert(nrf_orientation_estimator_update(&estimator, &vertical, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);

    assert(nrf_orientation_estimator_update(&estimator, &moving, true));
    assert(estimator.output.state == NRF_ORIENTATION_STATE_MOVING);

    for (unsigned i = 0U; i < 12U; ++i) {
        assert(nrf_orientation_estimator_update(&estimator, &tilted, true));
    }
    assert(estimator.output.state == NRF_ORIENTATION_STATE_OUTSIDE);

    for (unsigned i = 0U; i < 12U; ++i) {
        assert(nrf_orientation_estimator_update(&estimator, &vertical, true));
    }
    assert(estimator.output.state == NRF_ORIENTATION_STATE_ACCEPTED);
}

static void test_config_validation(void)
{
    nrf_orientation_config_t config;
    nrf_orientation_default_config(&config);
    assert(nrf_orientation_config_valid(&config));
    config.sample_period_ms = 0U;
    assert(!nrf_orientation_config_valid(&config));
    nrf_orientation_default_config(&config);
    config.reenter_sin2_ppm = config.accept_sin2_ppm + 1U;
    assert(!nrf_orientation_config_valid(&config));
}

int main(void)
{
    test_every_signed_axis();
    test_unconfigured_and_invalid_sample();
    test_hysteresis_and_boundary();
    test_exact_threshold_boundary();
    test_wrong_direction_and_severity();
    test_gravity_and_motion_gates();
    test_filter_convergence_and_recovery();
    test_config_validation();
    return 0;
}
