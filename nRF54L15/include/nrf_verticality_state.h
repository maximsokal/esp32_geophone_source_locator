#ifndef NRF_VERTICALITY_STATE_H
#define NRF_VERTICALITY_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_orientation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_VERTICALITY_DEFAULT_DWELL_MS 750U
#define NRF_VERTICALITY_DEFAULT_MOTION_EXIT_MS 200U
#define NRF_VERTICALITY_DEFAULT_MAX_SAMPLE_GAP_MS 300U

typedef enum {
    NRF_VERTICALITY_ACCEPTED = 0,
    NRF_VERTICALITY_STABILIZING,
    NRF_VERTICALITY_TILTED,
    NRF_VERTICALITY_FAULT,
} nrf_verticality_state_t;

typedef struct {
    uint32_t dwell_ms;
    uint32_t motion_exit_ms;
    uint32_t max_sample_gap_ms;
    uint32_t gyro_motion_threshold_mdps;
    uint32_t accept_sin2_ppm;
    uint32_t leave_sin2_ppm;
} nrf_verticality_config_t;

typedef struct {
    nrf_verticality_config_t config;
    nrf_verticality_state_t state;
    uint64_t last_sample_ms;
    uint64_t stable_since_ms;
    uint64_t motion_since_ms;
    bool have_sample;
    bool stable_since_valid;
    bool motion_since_valid;
} nrf_verticality_estimator_t;

void nrf_verticality_default_config(nrf_verticality_config_t *config);

bool nrf_verticality_config_valid(const nrf_verticality_config_t *config);

bool nrf_verticality_state_init(
    nrf_verticality_estimator_t *estimator,
    const nrf_verticality_config_t *config);

void nrf_verticality_state_reset(nrf_verticality_estimator_t *estimator);

nrf_verticality_state_t nrf_verticality_state_update(
    nrf_verticality_estimator_t *estimator,
    bool imu_valid,
    bool tilt_valid,
    uint32_t tilt_sin2_ppm,
    uint32_t filtered_gyro_motion_mdps,
    uint64_t sample_uptime_ms);

uint8_t nrf_verticality_state_to_orientation_wire(nrf_verticality_state_t state);

#ifdef __cplusplus
}
#endif

#endif
