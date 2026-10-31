#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_imu_calibration.h"

static nrf_imu_sample_t sample_for_axis(unsigned axis, int sign, int32_t gyro_x)
{
    nrf_imu_sample_t sample = {0};
    sample.accel_mg[axis] = sign * 1000;
    sample.gyro_mdps[0] = gyro_x;
    sample.gyro_mdps[1] = -200;
    sample.gyro_mdps[2] = 300;
    return sample;
}

static bool capture_window(
    nrf_imu_calibration_t *calibration,
    const nrf_imu_sample_t *sample,
    uint16_t generation)
{
    assert(nrf_imu_calibration_begin_capture(calibration, generation, 1000U));
    bool completed = false;
    for (uint64_t i = 0U; i <= 10U; ++i) {
        completed = nrf_imu_calibration_update(
            calibration, sample, true, false, 1000U + i * 100U);
    }
    return completed;
}

static void test_capture_bias_gravity_reference_and_all_axes(void)
{
    static const nrf_orientation_reference_axis_t expected_axes[] = {
        NRF_ORIENTATION_AXIS_POS_X,
        NRF_ORIENTATION_AXIS_NEG_X,
        NRF_ORIENTATION_AXIS_POS_Y,
        NRF_ORIENTATION_AXIS_NEG_Y,
        NRF_ORIENTATION_AXIS_POS_Z,
        NRF_ORIENTATION_AXIS_NEG_Z,
    };

    for (unsigned direction = 0U; direction < 6U; ++direction) {
        const unsigned axis = direction / 2U;
        const int sign = direction % 2U == 0U ? 1 : -1;
        nrf_imu_calibration_t calibration;
        const nrf_imu_sample_t sample = sample_for_axis(axis, sign, 14000);
        nrf_imu_calibration_init(&calibration);
        assert(capture_window(&calibration, &sample, (uint16_t)(direction + 1U)));
        assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
        assert(calibration.generation == direction + 1U);
        assert(calibration.reference_axis == expected_axes[direction]);
        assert(calibration.gyro_bias_mdps[0] == 14000);
        assert(calibration.gyro_bias_mdps[1] == -200);
        assert(calibration.gyro_bias_mdps[2] == 300);
        assert(calibration.gravity_ref_mg[axis] == sign * 1000);
        assert(calibration.gravity_ref_mg[(axis + 1U) % 3U] == 0);
        assert(calibration.gravity_ref_mg[(axis + 2U) % 3U] == 0);
    }
}

static void test_window_duration_and_gyro_bias_correction(void)
{
    nrf_imu_calibration_t calibration;
    nrf_imu_sample_t sample = sample_for_axis(2U, 1, 12000);
    int32_t corrected[3];

    nrf_imu_calibration_init(&calibration);
    assert(nrf_imu_calibration_begin_capture(&calibration, 65535U, 0U));
    for (uint64_t i = 0U; i < 10U; ++i) {
        assert(!nrf_imu_calibration_update(
            &calibration, &sample, true, false, i * 100U));
    }
    assert(calibration.capturing);
    assert(nrf_imu_calibration_update(
        &calibration, &sample, true, false, 1000U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(calibration.generation == UINT16_MAX);
    sample.gyro_mdps[0] = 13000;
    nrf_imu_calibration_correct_gyro_mdps(
        &calibration, sample.gyro_mdps, corrected);
    assert(corrected[0] == 1000);
    assert(corrected[1] == 0 && corrected[2] == 0);
}

static void test_capture_rejection_states_and_clear(void)
{
    nrf_imu_calibration_t calibration;
    nrf_imu_sample_t sample = sample_for_axis(2U, 1, 0);

    nrf_imu_calibration_init(&calibration);
    assert(nrf_imu_calibration_begin_capture(&calibration, 9U, 100U));
    assert(nrf_imu_calibration_update(
        &calibration, &sample, true, true, 100U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
    assert(!calibration.capturing && calibration.gyro_bias_mdps[0] == 0);

    sample = sample_for_axis(2U, 1, 0);
    assert(nrf_imu_calibration_begin_capture(&calibration, 10U, 1000U));
    bool completed = false;
    for (uint64_t i = 0U; i <= 10U; ++i) {
        sample.gyro_mdps[0] = i % 2U == 0U ? 2000 : -2000;
        completed = nrf_imu_calibration_update(
            &calibration, &sample, true, false, 1000U + i * 100U);
    }
    assert(completed);
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);

    sample = sample_for_axis(2U, 1, 0);
    sample.accel_mg[2] = 800;
    assert(nrf_imu_calibration_begin_capture(&calibration, 11U, 2000U));
    for (uint64_t i = 0U; i <= 10U; ++i) {
        completed = nrf_imu_calibration_update(
            &calibration, &sample, true, false, 2000U + i * 100U);
    }
    assert(completed);
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY);

    sample = sample_for_axis(2U, 1, 0);
    assert(nrf_imu_calibration_begin_capture(&calibration, 12U, 3000U));
    assert(!nrf_imu_calibration_update(
        &calibration, &sample, true, false, 3000U));
    assert(nrf_imu_calibration_update(
        &calibration, NULL, false, false, 3100U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);

    nrf_imu_calibration_clear(&calibration, 0U);
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED);
    assert(calibration.generation == 0U);
    assert(calibration.reference_axis == NRF_ORIENTATION_AXIS_UNCONFIGURED);
    assert(calibration.gravity_ref_mg[2] == 0 && calibration.gyro_bias_mdps[0] == 0);
}

static void test_every_failed_capture_reports_the_requested_generation(void)
{
    /* The ESP accepts a calibration result only when imu_cal_generation equals the generation it asked
     * for. A rejected or invalid capture used to keep the previous generation, so the ESP never saw
     * "rejected_moving" / "sensor_invalid" and the run ended only at its 15 s timeout. */
    nrf_imu_calibration_t calibration;
    nrf_imu_sample_t sample = sample_for_axis(2U, 1, 0);
    bool completed = false;

    nrf_imu_calibration_init(&calibration);
    assert(calibration.generation == 0U);

    assert(nrf_imu_calibration_begin_capture(&calibration, 21U, 100U));
    assert(nrf_imu_calibration_update(&calibration, &sample, true, true, 100U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
    assert(calibration.generation == 21U);
    assert(calibration.capture_generation == 0U);

    assert(nrf_imu_calibration_begin_capture(&calibration, 22U, 1000U));
    assert(nrf_imu_calibration_update(&calibration, NULL, false, false, 1100U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
    assert(calibration.generation == 22U);
    assert(calibration.capture_generation == 0U);

    sample = sample_for_axis(2U, 1, 0);
    sample.accel_mg[2] = 800;
    assert(nrf_imu_calibration_begin_capture(&calibration, 23U, 2000U));
    for (uint64_t i = 0U; i <= 10U; ++i) {
        completed = nrf_imu_calibration_update(&calibration, &sample, true, false, 2000U + i * 100U);
    }
    assert(completed);
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY);
    assert(calibration.generation == 23U);

    /* A good calibration followed by a failed one: the failure is attributed to its own request. */
    sample = sample_for_axis(2U, 1, 0);
    assert(capture_window(&calibration, &sample, 24U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(calibration.generation == 24U);
    assert(nrf_imu_calibration_begin_capture(&calibration, 25U, 9000U));
    assert(nrf_imu_calibration_update(&calibration, &sample, true, true, 9000U));
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
    assert(calibration.generation == 25U);
    assert(calibration.gyro_bias_mdps[0] == 0 && calibration.gravity_ref_mg[2] == 0);
}

static void test_stalled_capture_expires_with_its_generation(void)
{
    /* A capture is fed only by IMU samples. If the samples stop (IMU read stuck "pending", sensor lost
     * between reads) nothing ever finished it and the node stayed silent until the ESP's own timeout. */
    nrf_imu_calibration_t calibration;
    nrf_imu_calibration_init(&calibration);

    /* Nothing is running: nothing to expire. */
    assert(!nrf_imu_calibration_expire(&calibration, 100000U));

    assert(nrf_imu_calibration_begin_capture(&calibration, 31U, 1000U));
    assert(!nrf_imu_calibration_expire(&calibration, 1000U + NRF_IMU_CALIBRATION_TIMEOUT_MS - 1U));
    assert(calibration.capturing);
    assert(nrf_imu_calibration_expire(&calibration, 1000U + NRF_IMU_CALIBRATION_TIMEOUT_MS));
    assert(!calibration.capturing);
    assert(calibration.state == NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
    assert(calibration.generation == 31U && calibration.capture_generation == 0U);
    assert(calibration.gyro_bias_mdps[0] == 0 && calibration.gravity_ref_mg[2] == 0);
    assert(!nrf_imu_calibration_expire(&calibration, 900000U)); /* once */

    /* A clock that went backwards never expires a capture. */
    assert(nrf_imu_calibration_begin_capture(&calibration, 32U, 50000U));
    assert(!nrf_imu_calibration_expire(&calibration, 100U));
    assert(calibration.capturing);

    /* A normal capture is far inside the timeout: 1 s window, 100 Hz samples. */
    nrf_imu_calibration_t healthy;
    nrf_imu_sample_t sample = sample_for_axis(2U, 1, 0);
    nrf_imu_calibration_init(&healthy);
    assert(capture_window(&healthy, &sample, 33U));
    assert(healthy.state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(!nrf_imu_calibration_expire(&healthy, 1000U + NRF_IMU_CALIBRATION_TIMEOUT_MS));
}

static void test_four_instances_and_reboot_reset(void)
{
    nrf_imu_calibration_t nodes[4];
    for (unsigned i = 0U; i < 4U; ++i) {
        const nrf_imu_sample_t sample = sample_for_axis(i % 3U, 1, (int32_t)(100U + i));
        nrf_imu_calibration_init(&nodes[i]);
        assert(capture_window(&nodes[i], &sample, (uint16_t)(i + 1U)));
        assert(nodes[i].state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
        assert(nodes[i].gyro_bias_mdps[0] == (int16_t)(100U + i));
    }
    assert(nodes[0].generation == 1U && nodes[3].generation == 4U);
    assert(nodes[0].gravity_ref_mg[0] == 1000);
    assert(nodes[1].gravity_ref_mg[1] == 1000);
    assert(nodes[2].gravity_ref_mg[2] == 1000);
    assert(nodes[3].gravity_ref_mg[0] == 1000);

    nrf_imu_calibration_init(&nodes[0]);
    assert(nodes[0].state == NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED);
    assert(nodes[0].generation == 0U);
    assert(nodes[0].gyro_bias_mdps[0] == 0 && nodes[0].gravity_ref_mg[0] == 0);
    assert(nodes[1].state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED);
    assert(nodes[1].gyro_bias_mdps[0] == 101);
}

int main(void)
{
    test_capture_bias_gravity_reference_and_all_axes();
    test_window_duration_and_gyro_bias_correction();
    test_capture_rejection_states_and_clear();
    test_every_failed_capture_reports_the_requested_generation();
    test_stalled_capture_expires_with_its_generation();
    test_four_instances_and_reboot_reset();
    return 0;
}
