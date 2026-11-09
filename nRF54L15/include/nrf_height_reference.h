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

/*
 * The window describes one position: at least three altitudes and no more outliers than the centre may drop
 * (one in eight, at least one: a single glitch is noise). A node moved by a metre (12 Pa) leaves two or more
 * samples of its old position 25 sigma_r away -- the centre would then be the mean of two places.
 */
bool nrf_height_reference_window_settled(const int32_t *altitudes_mm, size_t count);

/*
 * The newest altitude lies beyond 3 sigma_r of the window median. One sample cannot tell a barometer glitch from the
 * start of a move the gyro did not see (a node lifted straight up): a capture waits for the next altitude.
 */
bool nrf_height_reference_window_newest_is_outlier(const int32_t *altitudes_mm, size_t count);

/*
 * A step: the two newest altitudes agree with each other (within 120 mm, 3 x the 40 mm sigma floor) and both lie beyond
 * 3 sigma_r of the samples before them. Two consistent readings of a new level are a move, not a glitch: the samples
 * before them describe the old place.
 */
bool nrf_height_reference_window_step(const int32_t *altitudes_mm, size_t count);

/*
 * While a capture waits, the node samples its barometer itself this often, so three altitudes of the settled place
 * exist within NRF_HEIGHT_REFERENCE_SETTLE_TIMEOUT_MS however rarely the ESP refreshes it: its 2 s refresh
 * (NRF_SENSOR_REFRESH_PERIOD_MS) is deferred during ranging and queued behind the other nodes. One forced conversion
 * takes 47 ms.
 */
#define NRF_HEIGHT_REFERENCE_CAPTURE_SAMPLE_PERIOD_MS 1000U

/*
 * A capture that arrives while the node is moving or its altitude window has not settled waits this long for it to
 * settle before it is REJECTED_MOVING. After a move the window needs three fresh refreshes (2 s apart) plus the
 * 750 ms verticality dwell; the answer still comes before the ESP's 15 s calibration timeout
 * (WORKFLOW_CALIBRATION_TIMEOUT_MS), so the row reads "rejected_moving", not "timeout".
 */
#define NRF_HEIGHT_REFERENCE_SETTLE_TIMEOUT_MS 12000U

/* A capture accepted but not decided yet: NOT_ESTABLISHED under the requested generation (the ESP keeps waiting). */
void nrf_height_reference_begin_pending(nrf_height_reference_t *reference, uint16_t generation);

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
