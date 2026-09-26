#include "nrf_verticality_state.h"

#include <stddef.h>
#include <string.h>

void nrf_verticality_default_config(nrf_verticality_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (nrf_verticality_config_t){
        .dwell_ms = NRF_VERTICALITY_DEFAULT_DWELL_MS,
        .motion_exit_ms = NRF_VERTICALITY_DEFAULT_MOTION_EXIT_MS,
        .max_sample_gap_ms = NRF_VERTICALITY_DEFAULT_MAX_SAMPLE_GAP_MS,
        .gyro_motion_threshold_mdps = NRF_ORIENTATION_DEFAULT_GYRO_MOTION_MDPS,
        .accept_sin2_ppm = NRF_ORIENTATION_DEFAULT_REENTER_SIN2_PPM,
        .leave_sin2_ppm = NRF_ORIENTATION_DEFAULT_ACCEPT_SIN2_PPM,
    };
}

bool nrf_verticality_config_valid(const nrf_verticality_config_t *config)
{
    return config != NULL && config->dwell_ms != 0U &&
           config->motion_exit_ms != 0U && config->max_sample_gap_ms != 0U &&
           config->gyro_motion_threshold_mdps != 0U &&
           config->accept_sin2_ppm <= config->leave_sin2_ppm &&
           config->leave_sin2_ppm <= 1000000U;
}

bool nrf_verticality_state_init(
    nrf_verticality_estimator_t *estimator,
    const nrf_verticality_config_t *config)
{
    if (estimator == NULL || !nrf_verticality_config_valid(config)) {
        return false;
    }
    memset(estimator, 0, sizeof(*estimator));
    estimator->config = *config;
    estimator->state = NRF_VERTICALITY_FAULT;
    return true;
}

void nrf_verticality_state_reset(nrf_verticality_estimator_t *estimator)
{
    if (estimator == NULL) {
        return;
    }
    estimator->state = NRF_VERTICALITY_FAULT;
    estimator->last_sample_ms = 0U;
    estimator->stable_since_ms = 0U;
    estimator->motion_since_ms = 0U;
    estimator->have_sample = false;
    estimator->stable_since_valid = false;
    estimator->motion_since_valid = false;
}

nrf_verticality_state_t nrf_verticality_state_update(
    nrf_verticality_estimator_t *estimator,
    bool imu_valid,
    bool tilt_valid,
    uint32_t tilt_sin2_ppm,
    uint32_t filtered_gyro_motion_mdps,
    uint64_t sample_uptime_ms)
{
    bool sample_gap = false;

    if (estimator == NULL) {
        return NRF_VERTICALITY_FAULT;
    }
    if (!imu_valid) {
        estimator->state = NRF_VERTICALITY_FAULT;
        estimator->stable_since_valid = false;
        estimator->motion_since_valid = false;
        estimator->have_sample = false;
        return estimator->state;
    }
    if (estimator->have_sample) {
        if (sample_uptime_ms <= estimator->last_sample_ms) {
            return estimator->state;
        }
        sample_gap = sample_uptime_ms - estimator->last_sample_ms >
            estimator->config.max_sample_gap_ms;
    }
    estimator->last_sample_ms = sample_uptime_ms;
    estimator->have_sample = true;
    if (sample_gap) {
        estimator->stable_since_valid = false;
        estimator->motion_since_valid = false;
        if (estimator->state == NRF_VERTICALITY_ACCEPTED) {
            estimator->state = NRF_VERTICALITY_STABILIZING;
        }
    }

    if (filtered_gyro_motion_mdps >=
        estimator->config.gyro_motion_threshold_mdps) {
        estimator->stable_since_valid = false;
        if (estimator->state == NRF_VERTICALITY_ACCEPTED) {
            if (!estimator->motion_since_valid) {
                estimator->motion_since_ms = sample_uptime_ms;
                estimator->motion_since_valid = true;
            }
            if (sample_uptime_ms - estimator->motion_since_ms >=
                estimator->config.motion_exit_ms) {
                estimator->state = NRF_VERTICALITY_STABILIZING;
                estimator->motion_since_valid = false;
            }
        } else {
            estimator->state = NRF_VERTICALITY_STABILIZING;
            estimator->motion_since_valid = false;
        }
        return estimator->state;
    }
    estimator->motion_since_valid = false;

    if (!tilt_valid || tilt_sin2_ppm > 1000000U) {
        estimator->state = NRF_VERTICALITY_FAULT;
        estimator->stable_since_valid = false;
        return estimator->state;
    }
    if (estimator->state == NRF_VERTICALITY_ACCEPTED) {
        if (tilt_sin2_ppm >= estimator->config.leave_sin2_ppm) {
            estimator->state = NRF_VERTICALITY_TILTED;
            estimator->stable_since_valid = false;
        }
        return estimator->state;
    }
    if (tilt_sin2_ppm > estimator->config.accept_sin2_ppm) {
        estimator->state = NRF_VERTICALITY_TILTED;
        estimator->stable_since_valid = false;
        return estimator->state;
    }
    if (!estimator->stable_since_valid) {
        estimator->stable_since_ms = sample_uptime_ms;
        estimator->stable_since_valid = true;
    }
    if (sample_uptime_ms - estimator->stable_since_ms >=
        estimator->config.dwell_ms) {
        estimator->state = NRF_VERTICALITY_ACCEPTED;
    } else {
        estimator->state = NRF_VERTICALITY_STABILIZING;
    }
    return estimator->state;
}

uint8_t nrf_verticality_state_to_orientation_wire(nrf_verticality_state_t state)
{
    switch (state) {
    case NRF_VERTICALITY_STABILIZING:
        return (uint8_t)NRF_ORIENTATION_STATE_MOVING;
    case NRF_VERTICALITY_ACCEPTED:
        return (uint8_t)NRF_ORIENTATION_STATE_ACCEPTED;
    case NRF_VERTICALITY_TILTED:
        return (uint8_t)NRF_ORIENTATION_STATE_OUTSIDE;
    case NRF_VERTICALITY_FAULT:
    default:
        return (uint8_t)NRF_ORIENTATION_STATE_UNAVAILABLE;
    }
}
