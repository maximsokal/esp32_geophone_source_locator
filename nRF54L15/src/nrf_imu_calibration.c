#include "nrf_imu_calibration.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define NRF_IMU_CALIBRATION_ACCEL_RANGE_MG 2000
#define NRF_IMU_CALIBRATION_GYRO_RANGE_MDPS 250000
#define NRF_IMU_CALIBRATION_GRAVITY_MIN_MG 850.0
#define NRF_IMU_CALIBRATION_GRAVITY_MAX_MG 1150.0

static void clear_result(nrf_imu_calibration_t *calibration)
{
    memset(calibration->gyro_bias_mdps, 0, sizeof(calibration->gyro_bias_mdps));
    memset(calibration->gravity_ref_mg, 0, sizeof(calibration->gravity_ref_mg));
    calibration->reference_axis = NRF_ORIENTATION_AXIS_UNCONFIGURED;
}

static void finish(nrf_imu_calibration_t *calibration, uint8_t state)
{
    calibration->state = (nrf_upstream_imu_calibration_state_t)state;
    calibration->capturing = false;
    clear_result(calibration);
    /*
     * The result belongs to the request that started this capture, whether it succeeded or not: the
     * ESP accepts a calibration state only when imu_cal_generation matches the generation it asked for.
     * Keeping the previous generation hid "rejected_moving" / "sensor_invalid" from the ESP, so a node
     * with a dead BMI160 looked like a silent node and the run ended only at its timeout.
     */
    calibration->generation = calibration->capture_generation;
    calibration->capture_generation = 0U;
}

static int64_t rounded_mean(int64_t sum, uint32_t count)
{
    const int64_t magnitude = sum < 0 ? -sum : sum;
    const int64_t rounded = (magnitude + (int64_t)(count / 2U)) / (int64_t)count;
    return sum < 0 ? -rounded : rounded;
}

static nrf_orientation_reference_axis_t gravity_axis(const int16_t gravity[3])
{
    size_t dominant = 0U;
    int32_t magnitude = gravity[0] < 0 ? -(int32_t)gravity[0] : gravity[0];
    for (size_t axis = 1U; axis < 3U; ++axis) {
        const int32_t candidate = gravity[axis] < 0
            ? -(int32_t)gravity[axis]
            : gravity[axis];
        if (candidate > magnitude) {
            magnitude = candidate;
            dominant = axis;
        }
    }
    if (dominant == 0U) {
        return gravity[0] < 0 ? NRF_ORIENTATION_AXIS_NEG_X : NRF_ORIENTATION_AXIS_POS_X;
    }
    if (dominant == 1U) {
        return gravity[1] < 0 ? NRF_ORIENTATION_AXIS_NEG_Y : NRF_ORIENTATION_AXIS_POS_Y;
    }
    return gravity[2] < 0 ? NRF_ORIENTATION_AXIS_NEG_Z : NRF_ORIENTATION_AXIS_POS_Z;
}

void nrf_imu_calibration_init(nrf_imu_calibration_t *calibration)
{
    if (calibration == NULL) {
        return;
    }
    memset(calibration, 0, sizeof(*calibration));
    calibration->state = NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED;
    calibration->reference_axis = NRF_ORIENTATION_AXIS_UNCONFIGURED;
}

bool nrf_imu_calibration_begin_capture(
    nrf_imu_calibration_t *calibration,
    uint16_t generation,
    uint64_t start_uptime_ms)
{
    if (calibration == NULL || calibration->capturing) {
        return false;
    }
    clear_result(calibration);
    memset(calibration->accel_sum, 0, sizeof(calibration->accel_sum));
    memset(calibration->gyro_sum, 0, sizeof(calibration->gyro_sum));
    memset(calibration->gyro_mean, 0, sizeof(calibration->gyro_mean));
    memset(calibration->gyro_m2, 0, sizeof(calibration->gyro_m2));
    calibration->state = NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED;
    calibration->capture_generation = generation;
    calibration->capture_started_ms = start_uptime_ms;
    calibration->sample_count = 0U;
    calibration->capturing = true;
    return true;
}

bool nrf_imu_calibration_expire(
    nrf_imu_calibration_t *calibration,
    uint64_t now_uptime_ms)
{
    if (calibration == NULL || !calibration->capturing ||
        now_uptime_ms < calibration->capture_started_ms ||
        now_uptime_ms - calibration->capture_started_ms < NRF_IMU_CALIBRATION_TIMEOUT_MS) {
        return false;
    }
    finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
    return true;
}

bool nrf_imu_calibration_update(
    nrf_imu_calibration_t *calibration,
    const nrf_imu_sample_t *sample,
    bool sample_valid,
    bool moving,
    uint64_t sample_uptime_ms)
{
    uint32_t count;
    double gravity_sq = 0.0;
    int16_t gravity[3];
    int16_t bias[3];

    if (calibration == NULL || !calibration->capturing) {
        return false;
    }
    if (!sample_valid || sample == NULL) {
        finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
        return true;
    }
    if (sample_uptime_ms < calibration->capture_started_ms) {
        finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
        return true;
    }
    if (moving) {
        finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
        return true;
    }
    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (sample->accel_mg[axis] < -NRF_IMU_CALIBRATION_ACCEL_RANGE_MG ||
            sample->accel_mg[axis] > NRF_IMU_CALIBRATION_ACCEL_RANGE_MG ||
            sample->gyro_mdps[axis] < -NRF_IMU_CALIBRATION_GYRO_RANGE_MDPS ||
            sample->gyro_mdps[axis] > NRF_IMU_CALIBRATION_GYRO_RANGE_MDPS) {
            finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
            return true;
        }
    }

    ++calibration->sample_count;
    count = calibration->sample_count;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        const double gyro = (double)sample->gyro_mdps[axis];
        const double delta = gyro - calibration->gyro_mean[axis];
        calibration->accel_sum[axis] += sample->accel_mg[axis];
        calibration->gyro_sum[axis] += sample->gyro_mdps[axis];
        calibration->gyro_mean[axis] += delta / (double)count;
        calibration->gyro_m2[axis] += delta * (gyro - calibration->gyro_mean[axis]);
    }

    if (sample_uptime_ms - calibration->capture_started_ms <
            NRF_IMU_CALIBRATION_WINDOW_MS ||
        count < NRF_IMU_CALIBRATION_MIN_SAMPLES) {
        return false;
    }
    for (size_t axis = 0U; axis < 3U; ++axis) {
        const double variance = calibration->gyro_m2[axis] / (double)count;
        const double max_variance =
            (double)NRF_IMU_CALIBRATION_GYRO_STDDEV_MAX_MDPS *
            (double)NRF_IMU_CALIBRATION_GYRO_STDDEV_MAX_MDPS;
        if (variance >= max_variance) {
            finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING);
            return true;
        }
    }

    for (size_t axis = 0U; axis < 3U; ++axis) {
        const int64_t accel_mean = rounded_mean(calibration->accel_sum[axis], count);
        const int64_t gyro_mean = rounded_mean(calibration->gyro_sum[axis], count);
        if (accel_mean < INT16_MIN || accel_mean > INT16_MAX ||
            gyro_mean < INT16_MIN || gyro_mean > INT16_MAX) {
            finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID);
            return true;
        }
        gravity[axis] = (int16_t)accel_mean;
        bias[axis] = (int16_t)gyro_mean;
        gravity_sq += (double)accel_mean * (double)accel_mean;
    }
    if (gravity_sq < NRF_IMU_CALIBRATION_GRAVITY_MIN_MG * NRF_IMU_CALIBRATION_GRAVITY_MIN_MG ||
        gravity_sq > NRF_IMU_CALIBRATION_GRAVITY_MAX_MG * NRF_IMU_CALIBRATION_GRAVITY_MAX_MG) {
        finish(calibration, NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY);
        return true;
    }

    memcpy(calibration->gyro_bias_mdps, bias, sizeof(bias));
    memcpy(calibration->gravity_ref_mg, gravity, sizeof(gravity));
    calibration->reference_axis = gravity_axis(gravity);
    calibration->generation = calibration->capture_generation;
    calibration->capture_generation = 0U;
    calibration->capturing = false;
    calibration->state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    return true;
}

void nrf_imu_calibration_clear(
    nrf_imu_calibration_t *calibration,
    uint16_t generation)
{
    if (calibration == NULL) {
        return;
    }
    memset(calibration, 0, sizeof(*calibration));
    calibration->state = NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED;
    calibration->reference_axis = NRF_ORIENTATION_AXIS_UNCONFIGURED;
    calibration->generation = generation;
}

void nrf_imu_calibration_correct_gyro_mdps(
    const nrf_imu_calibration_t *calibration,
    const int32_t gyro_mdps[3],
    int32_t corrected_mdps[3])
{
    if (gyro_mdps == NULL || corrected_mdps == NULL) {
        return;
    }
    for (size_t axis = 0U; axis < 3U; ++axis) {
        int64_t corrected = gyro_mdps[axis];
        if (calibration != NULL &&
            calibration->state == NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED) {
            corrected -= calibration->gyro_bias_mdps[axis];
        }
        if (corrected > INT32_MAX) {
            corrected = INT32_MAX;
        } else if (corrected < INT32_MIN) {
            corrected = INT32_MIN;
        }
        corrected_mdps[axis] = (int32_t)corrected;
    }
}
