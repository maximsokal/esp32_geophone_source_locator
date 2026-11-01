#ifndef NRF_HEIGHT_REFERENCE_H
#define NRF_HEIGHT_REFERENCE_H

#include <stdbool.h>
#include <stddef.h>
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

/* Altitudes of this many recent refreshes (2 s apart) feed the reference captured at a calibration. */
#define NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES 5U

/*
 * Centre of the recent altitudes for a reference capture: samples farther than 3 sigma_r from the median
 * (sigma_r = 1.4826 times the median absolute deviation, never below 40 mm) are outliers, the centre is the mean
 * of the others; if more than one in eight are outliers nothing is dropped. The reference used to be the single
 * filtered sample of the instant, whose error stayed in every height of the node for good. With fewer than three
 * samples (a node that has just started) there is nothing to take a centre of: `fallback_mm`.
 * The ESP32 uses the same estimator for the MAIN reference and for the node heights of the placement step.
 */
int32_t nrf_height_reference_window_centre_mm(
    const int32_t *altitudes_mm,
    size_t count,
    int32_t fallback_mm);

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
