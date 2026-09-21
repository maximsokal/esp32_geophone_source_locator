#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * MAX-139: an explicit, named propagation-speed model with an uncertainty
 * estimate, instead of a hidden/guessed constant. localization.c itself
 * still takes a plain propagation_speed_mps scalar (unchanged) -- this is
 * the layer that decides *what value* to put there and how much to trust
 * it, kept separate so every localization method can share the same
 * model/assumption rather than each guessing its own.
 *
 * "One or more candidate/region velocities" (rubble is not acoustically
 * uniform) is represented as a small named array a caller can pick from,
 * not a single project-wide hidden default -- this file never selects one
 * on the caller's behalf.
 */

#define LOCALIZATION_PROPAGATION_MODEL_NAME_SIZE 32U
#define LOCALIZATION_PROPAGATION_MAX_TAP_SAMPLES  64U


typedef struct
{
    char name[LOCALIZATION_PROPAGATION_MODEL_NAME_SIZE];
    double velocity_mps;
    double uncertainty_mps; /* 1-sigma; 0 means "unknown/uncalibrated", not "exact" */

} localization_propagation_model_t;


/*
 * Least-squares calibration from known-position tap/impact measurements:
 * for each sample i, known_distance_m[i] is the true distance from the tap
 * point to a reference sensor, and measured_travel_time_s[i] is that
 * sensor's observed arrival delay. Fits travel_time = distance/v + t0 (t0
 * absorbs a constant trigger/detection latency) by linear regression on
 * (distance, time), then derives velocity_mps = 1/slope and propagates the
 * slope's standard error into uncertainty_mps.
 *
 * Requires at least 3 samples (2 degrees of freedom) to produce a
 * meaningful uncertainty; returns ESP_ERR_INVALID_ARG for fewer. Returns
 * ESP_ERR_INVALID_STATE for a degenerate fit (near-zero or negative
 * implied velocity, or non-finite result) rather than reporting a
 * nonphysical number.
 */
esp_err_t localization_propagation_calibrate_from_taps(
    const double known_distance_m[],
    const double measured_travel_time_s[],
    size_t sample_count,
    const char *model_name,
    localization_propagation_model_t *result
);


/*
 * Empirical sensitivity: solves with config.propagation_speed_mps as the
 * assumed/baseline value, then re-solves with that value perturbed by
 * velocity_error_mps, and reports the resulting position delta in meters.
 * Same perturbation-against-the-real-solver approach as
 * localization_timing_jitter_error_bound() (MAX-142) -- this is an
 * empirical study, not a closed-form Jacobian approximation. Sign of
 * velocity_error_mps matters (assumed-too-fast vs assumed-too-slow are not
 * symmetric in general); callers wanting a worst-case bound should call
 * this twice with +/- the same magnitude.
 */
esp_err_t localization_propagation_velocity_error_bound(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_config_t *config,
    const localization_point3d_t *initial_guess,
    double velocity_error_mps,
    double *position_error_m_out
);
