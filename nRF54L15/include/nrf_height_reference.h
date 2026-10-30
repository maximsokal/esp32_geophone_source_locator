#ifndef NRF_HEIGHT_REFERENCE_H
#define NRF_HEIGHT_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A barometer glitch must not discard an established reference: relative Z holds its last good
 * value this long. Weather drift (a few cm per minute) only cancels against MAIN while both are
 * sampled live, so a longer outage reports SENSOR_INVALID until a fresh sample restores it.
 */
#define NRF_HEIGHT_REFERENCE_HOLD_MS 600000U

typedef struct {
    int32_t altitude_mm;
    uint16_t generation;
    nrf_upstream_reference_state_t state;
    int32_t last_relative_z_mm;
    uint64_t last_good_uptime_ms;
    bool has_last_good;
} nrf_height_reference_t;

void nrf_height_reference_init(nrf_height_reference_t *reference);

bool nrf_height_reference_apply(
    nrf_height_reference_t *reference,
    uint8_t action,
    uint16_t generation,
    bool imu_valid,
    bool env_valid,
    bool stationary,
    int32_t filtered_altitude_mm);

int32_t nrf_height_reference_relative_z_mm(
    const nrf_height_reference_t *reference,
    bool current_altitude_valid,
    int32_t filtered_altitude_mm);

/*
 * Wire view of the reference at now_ms. With a live altitude it returns the relative Z and
 * remembers it; without one it holds the last good Z until the hold expires.
 */
nrf_upstream_reference_state_t nrf_height_reference_sample(
    nrf_height_reference_t *reference,
    bool current_altitude_valid,
    int32_t filtered_altitude_mm,
    uint64_t now_ms,
    int32_t *relative_z_mm);

#ifdef __cplusplus
}
#endif

#endif
