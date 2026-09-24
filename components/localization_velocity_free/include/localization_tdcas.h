#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * TDCAS -- Three-Dimensional Comprehensive Analytical Solutions for
 * locating sources of sensor networks in an unknown-velocity system
 * (L. Dong, W. Shu, X. Li, G. Han, W. Zou, IEEE Access 5 (2017)
 * 11337-11351, IEEE Xplore document 7936463).
 *
 * P-wave arrival times only, no pre-measured velocity, no iteration, no
 * initial guess. For each sensor i with position s_i and arrival t_i:
 *
 *   |x - s_i|^2 = v^2 (t_i - t0)^2
 *
 * Subtracting the equation of a reference sensor r cancels |x|^2 and
 * v^2 t0^2, leaving an equation LINEAR in (x, y, z, A = v^2, B = v^2 t0):
 *
 *   2 (s_i - s_r) . x + (t_i^2 - t_r^2) A - 2 (t_i - t_r) B
 *       = |s_i|^2 - |s_r|^2
 *
 * Six sensors give five such equations and the explicit analytical
 * solution (no square root, so no root ambiguity). For more than six
 * sensors the "comprehensive" solution solves every 6-sensor combination
 * and fits a logistic probability density to each coordinate's
 * per-combination solutions; the fitted location parameter (the logistic
 * mode) is the final coordinate. The logistic maximum-likelihood location
 * down-weights outlying combinations (its score is bounded), so a single
 * bad pick spoils only the combinations that contain it.
 *
 * Near-planar surface arrays (MAX-211: geophones lie ON the surface) make
 * the z column of that system vanish. Such a layout is solved in its own
 * least-squares plane frame with 5-sensor combinations for (u, v, A, B);
 * the depth below the plane then follows from the recovered distances,
 * |w| = sqrt(A (t_i - t0)^2 - h_i^2), and the source is placed below the
 * surface (the only physical side for a buried source).
 *
 * Minimum sensors: 6 for a depth-resolving layout, 5 for a planar one.
 * This project's current 4-geophone hardware can therefore never satisfy
 * TDCAS; the result then says so (INSUFFICIENT_SENSORS) instead of
 * inventing a position.
 */

#define LOCALIZATION_TDCAS_MIN_SENSORS_3D     6U
#define LOCALIZATION_TDCAS_MIN_SENSORS_PLANAR 5U
#define LOCALIZATION_TDCAS_MAX_SENSORS        LOCALIZATION_MAX_SENSORS


typedef struct
{
    /* MAX-147: a combination whose solution the candidate filter rejects
     * (e.g. scan-verified FREE air) is discarded before the fusion. */
    localization_candidate_filter_t candidate_filter;

    /* A combination is discarded when any of its sensors' straight ray to
     * that combination's solution is obstructed (scan-verified empty space
     * on the ray): the straight-ray equations cannot hold for it. */
    localization_ray_filter_t ray_filter;

} localization_tdcas_config_t;


typedef enum
{
    LOCALIZATION_TDCAS_SOLVED = 0,

    /* Fewer sensors than the layout needs (6 in 3D, 5 when planar). */
    LOCALIZATION_TDCAS_INSUFFICIENT_SENSORS,

    /* Every combination was degenerate or nonphysical (v^2 <= 0, origin
     * time after an arrival, a negative squared depth). */
    LOCALIZATION_TDCAS_NO_VALID_COMBINATION,

    /* Valid combinations existed, but the candidate/ray filters rejected
     * every one of them. */
    LOCALIZATION_TDCAS_NO_ALLOWED_COMBINATION

} localization_tdcas_status_t;


typedef struct
{
    localization_tdcas_status_t status;

    /* True when the layout did not resolve depth and the planar-frame
     * variant (5-sensor combinations + recovered depth) was used. */
    bool planar_layout;

    size_t sensors_used;
    size_t combinations_total;
    size_t combinations_valid;
    size_t combinations_filtered;

    localization_point3d_t position;

    /* Logistic scale of the per-combination solutions, per axis (m), and
     * sqrt of the summed logistic variances -- how much the combinations
     * disagree. 0 with a single combination. */
    localization_point3d_t position_scale_m;
    double spread_m;

    /* Velocity and origin time re-fitted against ALL sensors at the final
     * position (t_i = t0 + |x - s_i| / v), and the RMS of that fit. */
    double velocity_mps;
    double velocity_scale_mps;
    int64_t emission_time_us;
    double rms_residual_us;

} localization_tdcas_result_t;


/*
 * observation_count must be in [1, LOCALIZATION_TDCAS_MAX_SENSORS]; config
 * may be NULL (no filters). Returns ESP_ERR_INVALID_ARG for NULL/invalid
 * input (non-finite coordinates). Data-driven failures are ESP_OK with a
 * non-SOLVED status.
 */
esp_err_t localization_tdcas_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_tdcas_config_t *config,
    localization_tdcas_result_t *result
);
