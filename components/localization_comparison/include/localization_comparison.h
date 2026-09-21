#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "localization.h"

/*
 * MAX-138 (part 4/4): combines already-computed per-method localization
 * outcomes -- the energy-only baseline (MAX-138 part 1), the existing
 * Gauss-Newton TDOA solver, the statistical traveltime inversion
 * (MAX-136), and the semblance/coherency backend (MAX-137), or any
 * subset/future addition -- into a single comparison, plus an optional
 * quality-weighted joint fusion.
 *
 * Deliberately knows nothing about any specific backend's own result
 * type or header: the caller adapts each backend's own quality/residual
 * measure into the common [0, 1] `quality` field before calling this,
 * keeping every backend in this epic decoupled from every other one, the
 * same way the solver itself stays unaware of every backend built on top
 * of it. Suggested per-method mappings (nothing here is hardcoded; every
 * number this component uses is caller-supplied):
 *
 *   - TDOA / traveltime inversion (both report a residual in
 *     microseconds): quality = exp(-0.5 * (residual_rms_us /
 *     timing_uncertainty_us)^2) -- the same caller-supplied-sigma
 *     Gaussian quality score localization_traveltime_inversion already
 *     uses internally to rank its own candidates.
 *   - Semblance/coherency: quality = best_semblance directly -- the
 *     Neidell & Taner score is already a coherence measure bounded in
 *     [0, 1] by construction.
 *   - Energy baseline: quality = energy_concentration -- a spread-out,
 *     ambiguous energy pattern is intrinsically a weaker position
 *     estimate than one clearly dominated by the nearest sensor.
 *
 * This component never overwrites the individual per-method outcomes it
 * is given -- they are copied verbatim into result->methods[] alongside
 * the separately-computed joint fusion, so disagreement between methods
 * stays directly visible (see also max_pairwise_disagreement_m) and a
 * caller/WebUI can always select and display any one method on its own.
 */

#define LOCALIZATION_COMPARISON_MAX_METHODS 8U
#define LOCALIZATION_COMPARISON_METHOD_NAME_SIZE 32U
#define LOCALIZATION_COMPARISON_REJECTION_REASON_SIZE 96U


typedef struct
{
    /* Never empty -- a nameless entry would be unselectable in a saved
     * session or a WebUI. */
    char method_name[LOCALIZATION_COMPARISON_METHOD_NAME_SIZE];

    bool solved;

    localization_point3d_t position; /* valid only if solved */

    /*
     * Caller-computed, in [0, 1]; higher = more trustworthy. Used as this
     * method's weight in the joint fusion. A solved method with quality
     * <= 0 still appears in result->methods[] and counts toward
     * max_pairwise_disagreement_m, but contributes nothing to the joint
     * fusion -- exactly how "a low-quality method cannot silently
     * dominate the joint result" is enforced: its weight is not silently
     * clamped to something nonzero.
     */
    double quality;

    /* Valid only if !solved -- never left blank for an unsolved method. */
    char rejection_reason[LOCALIZATION_COMPARISON_REJECTION_REASON_SIZE];

} localization_comparison_method_result_t;


typedef struct
{
    localization_comparison_method_result_t methods[LOCALIZATION_COMPARISON_MAX_METHODS];
    size_t method_count;

    bool joint_found;
    localization_point3d_t joint_position; /* quality-weighted average of every solved method */
    double joint_total_quality;            /* sum of the weights actually used; 0 if joint_found is false */
    size_t joint_methods_used_count;

    /*
     * Largest distance between any two SOLVED methods' positions
     * (regardless of quality/weight) -- 0 if fewer than 2 methods solved.
     * The concrete, reportable answer to "disagreement between methods is
     * visible".
     */
    double max_pairwise_disagreement_m;

} localization_comparison_result_t;


/*
 * method_count must be in [1, LOCALIZATION_COMPARISON_MAX_METHODS]. Every
 * entry must have a non-empty method_name; a solved entry must have a
 * finite position; an unsolved entry must have a non-empty
 * rejection_reason.
 */
esp_err_t localization_comparison_fuse(
    const localization_comparison_method_result_t *methods,
    size_t method_count,
    localization_comparison_result_t *result
);
