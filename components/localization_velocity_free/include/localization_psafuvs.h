#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * PSAFUVS -- MS/AE source location using arrival times of P and S waves
 * for an unknown-velocity system (L. Dong, X. Li, International Journal of
 * Distributed Sensor Networks 2013, article 307489,
 * doi:10.1155/2013/307489).
 *
 * No pre-measured velocity: the P and S velocities of THIS event are
 * estimated together with the source. With d_i the source distance of
 * sensor i and Dt_i = t_S,i - t_P,i its S-minus-P delay,
 *
 *   t_P,i = t0 + d_i / v_P,   t_S,i = t0 + d_i / v_S
 *   =>  d_i = K Dt_i,         K = v_P v_S / (v_P - v_S)
 *
 * so |x - s_i|^2 = K^2 Dt_i^2 holds for every sensor. Treating |x|^2 as
 * one more unknown R makes it linear in (x, y, z, K^2, R):
 *
 *   2 s_i . x + Dt_i^2 K^2 - R = |s_i|^2
 *
 * (equivalent to differencing against a reference sensor, without
 * privileging one sensor's pick noise). Five sensors solve it for a
 * depth-resolving layout. A near-planar surface array is solved in its
 * plane frame for (u, v, K^2, R') with four sensors -- this project's
 * current 4-geophone hardware -- and the depth below the surface follows
 * from the distances, as for TDCAS.
 *
 * The analytical solution then seeds a bounded Levenberg-Marquardt fit of
 * all 2N arrivals over (x, y, z, t0, 1/v_P, 1/v_S) -- the "fit" step: it
 * uses the P and S times directly instead of only their difference and
 * returns the event's own P/S velocities and origin time.
 */

#define LOCALIZATION_PSAFUVS_MIN_SENSORS_3D     5U
#define LOCALIZATION_PSAFUVS_MIN_SENSORS_PLANAR 4U
#define LOCALIZATION_PSAFUVS_MAX_SENSORS        LOCALIZATION_MAX_SENSORS


typedef struct
{
    localization_point3d_t sensor_position;
    int64_t p_arrival_time_us;
    /* Must be later than p_arrival_time_us. */
    int64_t s_arrival_time_us;

} localization_psafuvs_observation_t;


typedef struct
{
    /* Levenberg-Marquardt refinement; 0 keeps the analytical solution. */
    uint8_t max_iterations;
    double convergence_m;

    /* The final source must pass the candidate filter (MAX-147). */
    localization_candidate_filter_t candidate_filter;

    /* A sensor whose straight ray to the solution is obstructed is
     * excluded and the event re-solved from the remaining sensors (the
     * straight-ray equations cannot hold for it). */
    localization_ray_filter_t ray_filter;

} localization_psafuvs_config_t;


typedef enum
{
    LOCALIZATION_PSAFUVS_SOLVED = 0,

    /* Fewer sensors than the layout needs (5 in 3D, 4 when planar). */
    LOCALIZATION_PSAFUVS_INSUFFICIENT_SENSORS,

    /* Singular system, K^2 <= 0, or an inconsistent (negative) squared
     * depth: the arrivals do not describe one source. */
    LOCALIZATION_PSAFUVS_DEGENERATE,

    /* The fit is not physical: v_P <= 0 or v_S not below v_P. */
    LOCALIZATION_PSAFUVS_NONPHYSICAL_VELOCITIES,

    /* Excluding the obstructed sensors left too few to solve. */
    LOCALIZATION_PSAFUVS_RAYS_OBSTRUCTED,

    /* The solution lies where the candidate filter forbids a source. */
    LOCALIZATION_PSAFUVS_NO_ALLOWED_CANDIDATE

} localization_psafuvs_status_t;


typedef struct
{
    localization_psafuvs_status_t status;
    bool planar_layout;

    size_t sensors_used;
    uint32_t sensor_mask_used;       /* bit i: observation i used */
    uint32_t sensor_mask_obstructed; /* bit i: excluded by the ray filter */

    localization_point3d_t position;
    int64_t emission_time_us;
    double p_velocity_mps;
    double s_velocity_mps;
    double virtual_velocity_mps; /* K */

    /* RMS over all 2N P and S arrival residuals. */
    double rms_residual_us;
    uint8_t iterations;
    bool converged;

} localization_psafuvs_result_t;


localization_psafuvs_config_t localization_psafuvs_default_config(void);


/*
 * observation_count in [1, LOCALIZATION_PSAFUVS_MAX_SENSORS]; config may be
 * NULL (defaults, no filters). ESP_ERR_INVALID_ARG for NULL/non-finite
 * input or an S arrival not after its P arrival. Data-driven failures are
 * ESP_OK with a non-SOLVED status.
 */
esp_err_t localization_psafuvs_solve(
    const localization_psafuvs_observation_t *observations,
    size_t observation_count,
    const localization_psafuvs_config_t *config,
    localization_psafuvs_result_t *result
);
