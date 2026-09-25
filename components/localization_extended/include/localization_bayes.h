#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_extended.h"

/*
 * Bayesian source location: the posterior probability of the source over
 * a lattice of candidate positions, instead of one point.
 *
 *   p(x | t) ~ prior(x) * integral p(t | x, t0, v) p(v) dt0 dv
 *
 * The origin time t0 has a flat prior and is integrated analytically
 * (Gaussian likelihood) or profiled (Student-t); the velocity v has a
 * Gaussian prior (the caller's model speed +- its uncertainty) integrated
 * by Gauss-Hermite quadrature. prior(x) is uniform over the admitted
 * candidates: the candidate filter (e.g. "not in scan-verified air") is
 * the prior's support. Travel times are straight rays, or any travel-time
 * model (e.g. eikonal fields around scan voids) at the reference velocity.
 *
 * The lattice is evaluated in levels, each zooming onto the previous
 * level's 99.9% region with a likelihood tempered to that level's
 * resolution, so a posterior far narrower than the search box (or a long
 * thin ridge) is resolved instead of being sampled by luck.
 *
 * Reported: the MAP position (refined on a finer local lattice), the
 * posterior mean and per-axis standard deviations (including the lattice's
 * own quantization), the posterior of the velocity, and the smallest
 * credible region holding `credible_level` of the probability -- its
 * volume (area for a planar search) and bounding box. A wide region is the
 * honest answer when the geometry cannot resolve a direction (depth under
 * a flat array, say).
 */

typedef enum
{
    LOCALIZATION_BAYES_GAUSSIAN = 0,
    /* Heavy tails: a single wrong pick lowers the likelihood only
     * logarithmically instead of quadratically. */
    LOCALIZATION_BAYES_STUDENT_T

} localization_bayes_likelihood_t;


typedef struct
{
    localization_point3d_t sensor_position;
    int64_t arrival_time_us;
    double weight;                  /* precision multiplier; 0 = not used */

} localization_bayes_pick_t;


typedef struct
{
    double velocity_mps;            /* prior mean; the reference velocity */
    double velocity_sigma_mps;      /* prior sigma; 0 = known exactly */
    double timing_uncertainty_us;

    localization_bayes_likelihood_t likelihood;
    double student_nu;              /* <= 0: 4 */
    double credible_level;          /* (0, 1); 0: 0.9 */
    size_t velocity_nodes;          /* odd, <= 9; 0: 7 */
    size_t max_cells;               /* 0: 40000 */
    uint32_t refine_divisor;        /* >= 2 */

    localization_candidate_filter_t candidate_filter;
    localization_travel_time_model_t travel_time_model;

} localization_bayes_config_t;


typedef enum
{
    LOCALIZATION_BAYES_SOLVED = 0,
    LOCALIZATION_BAYES_INSUFFICIENT_PICKS,
    LOCALIZATION_BAYES_NO_ALLOWED_CANDIDATE,
    LOCALIZATION_BAYES_NO_REACHABLE_CANDIDATE

} localization_bayes_status_t;


typedef struct
{
    localization_bayes_status_t status;
    bool planar;

    localization_point3d_t map_position;
    localization_point3d_t mean_position;
    localization_point3d_t std_m;           /* per axis (z = 0 when planar) */

    double credible_level;
    double credible_volume_m3;              /* 3D */
    double credible_area_m2;                /* planar */
    localization_point3d_t credible_min;
    localization_point3d_t credible_max;
    size_t credible_cells;

    double map_cell_probability;            /* posterior mass of the MAP cell */
    double velocity_mean_mps;               /* posterior */
    double velocity_std_mps;
    int64_t emission_time_us;               /* at the MAP, posterior-mean velocity */
    double rms_residual_us;

    size_t picks_used;
    uint32_t pick_mask_used;
    size_t cells_evaluated;
    double cell_m;                          /* the finest level's lattice step */
    /* The pick sigma the finest level could resolve: the configured one,
     * or larger (a wider, conservative region) when the cell budget ran
     * out before the lattice matched the posterior's width. */
    double timing_uncertainty_used_us;

} localization_bayes_result_t;


/*
 * pick_count in [1, LOCALIZATION_EXTENDED_MAX_SENSORS]. With a travel-time
 * model, pick i is that model's sensor i. ESP_ERR_INVALID_ARG for invalid
 * input, ESP_ERR_NO_MEM when the posterior lattice cannot be allocated,
 * ESP_OK otherwise (see result->status).
 */
esp_err_t localization_bayes_locate(
    const localization_bayes_pick_t *picks,
    size_t pick_count,
    const localization_extended_bounds_t *bounds,
    const localization_bayes_config_t *config,
    localization_bayes_result_t *result
);
