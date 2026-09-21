#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_dimensionality_policy.h"

/*
 * MAX-136: an independent source-location backend using a statistical
 * traveltime-inversion grid search -- inspired by the technique family
 * described in Arosio et al. (2009): grid-search over candidate source
 * positions, scoring each by how well it predicts the OBSERVED
 * differential arrival times (one pick's arrival time minus a shared
 * reference pick's arrival time), rather than iteratively solving a
 * nonlinear least-squares system like the existing Gauss-Newton TDOA
 * solver (localization_tdoa_solve(), never called from here). Deliberately
 * independent so a report/WebUI can show both methods' results side by
 * side, per the ticket's own requirement.
 *
 * Consumes the SAME localization_observation_t (sensor_position +
 * arrival_time_us) fed to the existing solver -- "feed the same event
 * picks/sensor geometry" -- wrapped with a per-pick weight so a bad/missing
 * pick can be excluded (weight <= 0) or down-weighted, without inventing a
 * new pick format.
 *
 * Unlike a point-estimate solver, this backend retains the coarse-grid
 * residual/likelihood SURFACE (one entry per coarse candidate), not only
 * the single best position -- a caller that wants an uncertainty estimate
 * (a confidence region, not just a point) reads result->surface directly.
 * The refinement pass only sharpens the point estimate; it is not added to
 * the retained surface, keeping its memory bounded by the caller's own
 * chosen coarse grid resolution.
 *
 * propagation_speed_mps and timing_uncertainty_us are always
 * caller-supplied: there is no universal speed or pick-timing noise
 * figure, and inventing either here would silently bias every position and
 * likelihood value this backend reports.
 */

#define LOCALIZATION_TRAVELTIME_MAX_PICKS 8U


typedef struct
{
    localization_observation_t observation;

    /* <= 0 excludes this pick from the inversion entirely (a bad/missing
     * arrival). Otherwise a relative confidence weight; 1.0 is a normal
     * pick. The most-trusted (highest-weight) included pick is used as the
     * differential-time reference, so its own noise affects every residual
     * least. */
    double weight;

} localization_traveltime_pick_t;


typedef struct
{
    localization_point3d_t min_corner;
    localization_point3d_t max_corner;
    double step_m;

    /* Set min_corner.z_m == max_corner.z_m for a 2D (planar) search -- the
     * same code path naturally degenerates to a single-layer grid. */

} localization_traveltime_search_bounds_t;


typedef struct
{
    double propagation_speed_mps;

    /* Assumed 1-sigma arrival-time pick noise, in microseconds (e.g. from
     * MAX-142's empirical timing-jitter finding). Must come from the
     * caller's own measurement/calibration. */
    double timing_uncertainty_us;

    /* Coarse pass uses search_bounds.step_m; the refinement pass searches
     * +/- one coarse step around the coarse best point at
     * step_m / refine_divisor (>= 2). */
    uint32_t refine_divisor;

} localization_traveltime_config_t;


typedef struct
{
    localization_point3d_t position;
    double residual_rms_us; /* unweighted RMS of differential-time residuals */

    /*
     * exp(-0.5 * (this candidate's weighted chi-square - the best chi-square
     * found anywhere in the search, coarse or refined)) -- 1.0 only if this
     * exact candidate IS the global best; otherwise how much less likely it
     * is RELATIVE to that best. Deliberately not an absolute/normalized PDF
     * value: at realistic timing precision, an absolute exp(-0.5*chi-square)
     * underflows to indistinguishable zeros across almost the whole grid,
     * which would make the surface useless for comparing candidates.
     */
    double relative_likelihood;

} localization_traveltime_candidate_t;


typedef enum
{
    LOCALIZATION_TRAVELTIME_SOLVED = 0,

    /* Fewer than LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D usable
     * (weight > 0) picks: no method, statistical or otherwise, can run. */
    LOCALIZATION_TRAVELTIME_INSUFFICIENT_PICKS

} localization_traveltime_status_t;


typedef struct
{
    localization_traveltime_status_t status;
    localization_dimensionality_t dimensionality;
    size_t picks_used_count;

    localization_point3d_t best_position;
    double best_residual_rms_us; /* absolute fit quality of best_position, in microseconds */

    /*
     * The retained coarse-grid residual/likelihood surface: one entry per
     * coarse candidate searched, in scan order. The caller owns this memory
     * and must free(result->surface) exactly once when status == SOLVED
     * (NULL/0 otherwise -- nothing to free).
     */
    localization_traveltime_candidate_t *surface;
    size_t surface_count;

} localization_traveltime_result_t;


/*
 * pick_count must be in [1, LOCALIZATION_TRAVELTIME_MAX_PICKS]. Picks with
 * weight <= 0 are excluded before the dimensionality check, so a caller may
 * always pass its full physically-wired channel set and let this backend
 * decide which are usable.
 */
esp_err_t localization_traveltime_search(
    const localization_traveltime_pick_t *picks,
    size_t pick_count,
    const localization_traveltime_search_bounds_t *coarse_bounds,
    const localization_traveltime_config_t *config,
    localization_traveltime_result_t *result
);
