#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_extended.h"

/*
 * PSP: joint P-TDOA + P-S source location. Two complementary constraints
 * are fitted together:
 *
 *   P arrivals across sensors   t_P,i = t0 + p D_i          (p = 1 / vP)
 *   S - P on each sensor        t_S,i - t_P,i = q D_i        (q = 1/vS - 1/vP)
 *
 * with D_i = |x - s_i|. The P-S interval is independent of the origin time
 * and scales with distance alone, so it pins the range that P differences
 * leave loose (depth under a surface array in particular), while the P
 * differences fix the direction.
 *
 * Unlike PSAFUVS it needs no closed-form start (a lattice seeds the fit),
 * does not need an S pick on every sensor, and weights each P-S interval by
 * its own uncertainty: a doubtful S pick (low SNR) is given a large sigma
 * and so a small say, instead of being either trusted fully or thrown away.
 * vP may be fixed, given as a prior, or left free; vS is always estimated.
 */

typedef struct
{
    localization_point3d_t sensor_position;
    int64_t p_arrival_time_us;

    bool s_valid;
    int64_t s_arrival_time_us;
    /* Standard deviation of this sensor's S - P interval; <= 0 selects
     * sqrt(2) x the P timing uncertainty (two equally good picks). */
    double s_minus_p_sigma_us;

} localization_psp_observation_t;


typedef struct
{
    double timing_uncertainty_us;       /* one P pick's standard deviation */

    /* vP: sigma < 0 leaves it free, 0 fixes it at p_velocity_mps, > 0 is a
     * Gaussian prior. */
    double p_velocity_mps;
    double p_velocity_sigma_mps;

    size_t max_lattice_points;          /* seeding lattice; 0: 8000 */
    size_t max_iterations;              /* Levenberg-Marquardt; 0: 60 */

    localization_candidate_filter_t candidate_filter;

} localization_psp_config_t;


typedef enum
{
    LOCALIZATION_PSP_SOLVED = 0,
    LOCALIZATION_PSP_INSUFFICIENT_OBSERVATIONS,
    LOCALIZATION_PSP_NO_S_PICK,
    LOCALIZATION_PSP_NO_ALLOWED_CANDIDATE,
    LOCALIZATION_PSP_NONPHYSICAL_VELOCITIES,
    /* The data admit a family of equally good solutions -- e.g. four
     * coplanar sensors on one circle (a rectangle, an isosceles trapezoid)
     * with vP free: depth trades against velocity. The best-fitting member
     * is still reported in position, but it is not a location. A vP prior,
     * a layout off the circle or a fifth sensor resolves it. */
    LOCALIZATION_PSP_UNDERDETERMINED

} localization_psp_status_t;


typedef struct
{
    localization_psp_status_t status;
    bool planar;

    localization_point3d_t position;
    int64_t emission_time_us;
    double p_velocity_mps;
    double s_velocity_mps;

    double rms_p_residual_us;
    double rms_s_minus_p_residual_us;
    size_t p_used;
    size_t s_used;
    uint32_t s_mask_used;               /* bit i: observations[i] */

    /* rows (P picks + S-P intervals + a vP prior) - unknowns. */
    int32_t degrees_of_freedom;
    size_t iterations;
    bool converged;
    /* Smallest / largest eigenvalue of the equilibrated normal matrix at
     * the solution; below 1e-12 the status is UNDERDETERMINED. */
    double normal_rcond;

} localization_psp_result_t;


/*
 * observation_count in [1, LOCALIZATION_EXTENDED_MAX_SENSORS]; every S
 * pick must be later than its P pick. ESP_ERR_INVALID_ARG for invalid
 * input, ESP_OK otherwise (see result->status).
 */
esp_err_t localization_psp_solve(
    const localization_psp_observation_t *observations,
    size_t observation_count,
    const localization_extended_bounds_t *bounds,
    const localization_psp_config_t *config,
    localization_psp_result_t *result
);
