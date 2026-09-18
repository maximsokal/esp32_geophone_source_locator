#include "nrf_orientation.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define NRF_ORIENTATION_PPM_SCALE UINT64_C(1000000)

static uint32_t abs_i32_to_u32(int32_t value)
{
    if (value >= 0) {
        return (uint32_t)value;
    }
    return (uint32_t)(-(int64_t)value);
}

static uint32_t max_gyro_abs(const nrf_imu_sample_t *sample)
{
    uint32_t peak = 0U;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        const uint32_t value = abs_i32_to_u32(sample->gyro_mdps[axis]);
        if (value > peak) {
            peak = value;
        }
    }
    return peak;
}

static uint64_t accel_magnitude_sq(const int32_t accel_mg[3])
{
    uint64_t sum = 0U;
    for (size_t axis = 0U; axis < 3U; ++axis) {
        const int64_t value = accel_mg[axis];
        sum += (uint64_t)(value * value);
    }
    return sum;
}

static int64_t reference_projection(
    nrf_orientation_reference_axis_t axis,
    const int32_t accel_mg[3])
{
    switch (axis) {
    case NRF_ORIENTATION_AXIS_POS_X:
        return accel_mg[0];
    case NRF_ORIENTATION_AXIS_NEG_X:
        return -accel_mg[0];
    case NRF_ORIENTATION_AXIS_POS_Y:
        return accel_mg[1];
    case NRF_ORIENTATION_AXIS_NEG_Y:
        return -accel_mg[1];
    case NRF_ORIENTATION_AXIS_POS_Z:
        return accel_mg[2];
    case NRF_ORIENTATION_AXIS_NEG_Z:
        return -accel_mg[2];
    case NRF_ORIENTATION_AXIS_UNCONFIGURED:
    default:
        return 0;
    }
}

static int32_t ema_step(int32_t filtered, int32_t sample, uint8_t shift)
{
    const int64_t delta = (int64_t)sample - (int64_t)filtered;
    if (shift == 0U) {
        return sample;
    }
    const int64_t half = INT64_C(1) << (shift - 1U);
    const int64_t adjusted = delta >= 0 ? delta + half : delta - half;
    const int64_t divisor = INT64_C(1) << shift;
    return (int32_t)((int64_t)filtered + adjusted / divisor);
}

static void clear_output(
    nrf_orientation_estimator_t *estimator,
    nrf_orientation_state_t state,
    nrf_orientation_reason_t reason)
{
    nrf_orientation_output_t *output = &estimator->output;
    output->sample_valid = false;
    output->state = state;
    output->reason = reason;
    output->blink_class = NRF_ORIENTATION_BLINK_NONE;
    output->tilt_sin2_ppm = 0U;
    output->gravity_sq_mg2 = 0U;
    output->gyro_peak_mdps = 0U;
    memset(output->filtered_accel_mg, 0, sizeof(output->filtered_accel_mg));
}

void nrf_orientation_default_config(nrf_orientation_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (nrf_orientation_config_t){
        .reference_axis = NRF_ORIENTATION_AXIS_UNCONFIGURED,
        .sample_period_ms = NRF_ORIENTATION_DEFAULT_SAMPLE_PERIOD_MS,
        .gyro_motion_threshold_mdps = NRF_ORIENTATION_DEFAULT_GYRO_MOTION_MDPS,
        .gravity_min_mg = NRF_ORIENTATION_DEFAULT_GRAVITY_MIN_MG,
        .gravity_max_mg = NRF_ORIENTATION_DEFAULT_GRAVITY_MAX_MG,
        .accept_sin2_ppm = NRF_ORIENTATION_DEFAULT_ACCEPT_SIN2_PPM,
        .reenter_sin2_ppm = NRF_ORIENTATION_DEFAULT_REENTER_SIN2_PPM,
        .medium_sin2_ppm = NRF_ORIENTATION_DEFAULT_MEDIUM_SIN2_PPM,
        .fast_sin2_ppm = NRF_ORIENTATION_DEFAULT_FAST_SIN2_PPM,
        .ema_shift = NRF_ORIENTATION_DEFAULT_EMA_SHIFT,
    };
}

bool nrf_orientation_config_valid(const nrf_orientation_config_t *config)
{
    if (config == NULL || config->sample_period_ms == 0U ||
        config->gravity_min_mg == 0U ||
        config->gravity_min_mg >= config->gravity_max_mg ||
        config->accept_sin2_ppm > NRF_ORIENTATION_PPM_SCALE ||
        config->reenter_sin2_ppm > config->accept_sin2_ppm ||
        config->medium_sin2_ppm < config->accept_sin2_ppm ||
        config->fast_sin2_ppm < config->medium_sin2_ppm ||
        config->fast_sin2_ppm > NRF_ORIENTATION_PPM_SCALE ||
        config->ema_shift > 8U ||
        config->reference_axis < NRF_ORIENTATION_AXIS_UNCONFIGURED ||
        config->reference_axis > NRF_ORIENTATION_AXIS_NEG_Z) {
        return false;
    }
    return true;
}

bool nrf_orientation_estimator_init(
    nrf_orientation_estimator_t *estimator,
    const nrf_orientation_config_t *config)
{
    if (estimator == NULL || !nrf_orientation_config_valid(config)) {
        return false;
    }
    memset(estimator, 0, sizeof(*estimator));
    estimator->config = *config;
    clear_output(
        estimator,
        NRF_ORIENTATION_STATE_UNAVAILABLE,
        config->reference_axis == NRF_ORIENTATION_AXIS_UNCONFIGURED
            ? NRF_ORIENTATION_REASON_UNCONFIGURED
            : NRF_ORIENTATION_REASON_SAMPLE_INVALID);
    return true;
}

void nrf_orientation_estimator_invalidate(
    nrf_orientation_estimator_t *estimator,
    nrf_orientation_reason_t reason)
{
    if (estimator == NULL) {
        return;
    }
    estimator->accepted_latched = false;
    estimator->filter_initialized = false;
    memset(estimator->filtered_accel_mg, 0, sizeof(estimator->filtered_accel_mg));
    clear_output(estimator, NRF_ORIENTATION_STATE_UNAVAILABLE, reason);
}

/*
 * Blink rate is a "getting warmer" signal, not a danger signal: the closer
 * the node is to the accepted vertical band (without yet being inside it),
 * the FASTER the blink, culminating in a steady light once ACCEPTED. A
 * large tilt error (or a wrong-direction/upside-down reading) is therefore
 * the SLOWEST outside class, not the fastest.
 */
static nrf_orientation_blink_class_t outside_blink_class(
    const nrf_orientation_config_t *config,
    uint32_t tilt_sin2_ppm,
    bool wrong_direction)
{
    if (wrong_direction || tilt_sin2_ppm >= config->fast_sin2_ppm) {
        return NRF_ORIENTATION_BLINK_SLOW;
    }
    if (tilt_sin2_ppm >= config->medium_sin2_ppm) {
        return NRF_ORIENTATION_BLINK_MEDIUM;
    }
    return NRF_ORIENTATION_BLINK_FAST;
}

bool nrf_orientation_estimator_update(
    nrf_orientation_estimator_t *estimator,
    const nrf_imu_sample_t *sample,
    bool sample_valid)
{
    uint64_t raw_mag_sq;
    uint64_t filtered_mag_sq;
    uint64_t projection_sq;
    uint64_t transverse_sq;
    uint64_t gravity_min_sq;
    uint64_t gravity_max_sq;
    uint32_t gyro_peak;
    int64_t projection;
    uint32_t tilt_sin2_ppm;
    bool accepted;

    if (estimator == NULL || sample == NULL) {
        return false;
    }
    if (estimator->config.reference_axis == NRF_ORIENTATION_AXIS_UNCONFIGURED) {
        nrf_orientation_estimator_invalidate(
            estimator,
            NRF_ORIENTATION_REASON_UNCONFIGURED);
        return false;
    }
    if (!sample_valid) {
        nrf_orientation_estimator_invalidate(
            estimator,
            NRF_ORIENTATION_REASON_SAMPLE_INVALID);
        return false;
    }

    raw_mag_sq = accel_magnitude_sq(sample->accel_mg);
    gravity_min_sq = (uint64_t)estimator->config.gravity_min_mg *
                     (uint64_t)estimator->config.gravity_min_mg;
    gravity_max_sq = (uint64_t)estimator->config.gravity_max_mg *
                     (uint64_t)estimator->config.gravity_max_mg;
    if (raw_mag_sq < gravity_min_sq || raw_mag_sq > gravity_max_sq) {
        nrf_orientation_estimator_invalidate(
            estimator,
            NRF_ORIENTATION_REASON_GRAVITY_RANGE);
        estimator->output.gravity_sq_mg2 = raw_mag_sq > UINT32_MAX
            ? UINT32_MAX
            : (uint32_t)raw_mag_sq;
        return false;
    }

    gyro_peak = max_gyro_abs(sample);
    if (gyro_peak > estimator->config.gyro_motion_threshold_mdps) {
        clear_output(
            estimator,
            NRF_ORIENTATION_STATE_MOVING,
            NRF_ORIENTATION_REASON_MOTION);
        estimator->output.gyro_peak_mdps = gyro_peak;
        estimator->output.gravity_sq_mg2 = (uint32_t)raw_mag_sq;
        estimator->accepted_latched = false;
        estimator->filter_initialized = false;
        memset(estimator->filtered_accel_mg, 0, sizeof(estimator->filtered_accel_mg));
        return true;
    }

    if (!estimator->filter_initialized) {
        memcpy(
            estimator->filtered_accel_mg,
            sample->accel_mg,
            sizeof(estimator->filtered_accel_mg));
        estimator->filter_initialized = true;
    } else {
        for (size_t axis = 0U; axis < 3U; ++axis) {
            estimator->filtered_accel_mg[axis] = ema_step(
                estimator->filtered_accel_mg[axis],
                sample->accel_mg[axis],
                estimator->config.ema_shift);
        }
    }

    filtered_mag_sq = accel_magnitude_sq(estimator->filtered_accel_mg);
    if (filtered_mag_sq == 0U) {
        nrf_orientation_estimator_invalidate(
            estimator,
            NRF_ORIENTATION_REASON_GRAVITY_RANGE);
        return false;
    }

    projection = reference_projection(
        estimator->config.reference_axis,
        estimator->filtered_accel_mg);
    if (projection <= 0) {
        tilt_sin2_ppm = 1000000U;
        accepted = false;
    } else {
        projection_sq = (uint64_t)(projection * projection);
        transverse_sq = filtered_mag_sq > projection_sq
            ? filtered_mag_sq - projection_sq
            : 0U;
        tilt_sin2_ppm = (uint32_t)((transverse_sq * NRF_ORIENTATION_PPM_SCALE +
                                    filtered_mag_sq / 2U) /
                                   filtered_mag_sq);
        if (tilt_sin2_ppm > 1000000U) {
            tilt_sin2_ppm = 1000000U;
        }
        accepted = estimator->accepted_latched
            ? tilt_sin2_ppm <= estimator->config.accept_sin2_ppm
            : tilt_sin2_ppm <= estimator->config.reenter_sin2_ppm;
    }

    estimator->accepted_latched = accepted;
    estimator->output.sample_valid = true;
    estimator->output.state = accepted
        ? NRF_ORIENTATION_STATE_ACCEPTED
        : NRF_ORIENTATION_STATE_OUTSIDE;
    estimator->output.reason = NRF_ORIENTATION_REASON_NONE;
    estimator->output.blink_class = accepted
        ? NRF_ORIENTATION_BLINK_NONE
        : outside_blink_class(
              &estimator->config,
              tilt_sin2_ppm,
              projection <= 0);
    estimator->output.tilt_sin2_ppm = tilt_sin2_ppm;
    estimator->output.gravity_sq_mg2 = filtered_mag_sq > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)filtered_mag_sq;
    estimator->output.gyro_peak_mdps = gyro_peak;
    memcpy(
        estimator->output.filtered_accel_mg,
        estimator->filtered_accel_mg,
        sizeof(estimator->output.filtered_accel_mg));
    return true;
}

void nrf_orientation_estimator_get(
    const nrf_orientation_estimator_t *estimator,
    nrf_orientation_output_t *output)
{
    if (estimator == NULL || output == NULL) {
        return;
    }
    *output = estimator->output;
}
