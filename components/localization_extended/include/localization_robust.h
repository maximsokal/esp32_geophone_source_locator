#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_extended.h"

/*
 * Robust arrival-time source location: an M-estimator (Huber or Tukey
 * bisquare, iteratively reweighted) seeded by an exhaustive minimal-subset
 * consensus (MSAC, the RANSAC variant that scores truncated squared
 * residuals). One wrong pick -- a reflection taken as the first arrival,
 * a noise trigger -- pulls an ordinary least-squares fit arbitrarily far;
 * here it is identified and dropped, or down-weighted.
 *
 * Model: t_i = t0 + |x - s_i| / v with the caller's velocity. Unknowns:
 * (x, y, z, t0), or (x, y, t0) for a planar search (bounds with
 * min z == max z). A minimal subset therefore has 4 picks (3 planar), and
 * a bad pick can only be *identified* with at least one more than that:
 * with exactly one spare pick every subset that swaps the outlier for a
 * good pick fits equally well. The result then says so
 * (consensus_ambiguous) and falls back to a Huber fit over every pick
 * instead of guessing which one is wrong.
 */

typedef struct
{
    double propagation_speed_mps;
    double timing_uncertainty_us;       /* one pick's standard deviation */

    localization_extended_loss_t loss;
    double tuning_k;                    /* <= 0: the loss's default constant */
    double inlier_threshold_sigma;      /* <= 0: 3 */
    bool consensus;                     /* run the minimal-subset consensus */

    size_t max_lattice_points;          /* seeding lattice; 0: 8000 */
    size_t max_iterations;              /* IRLS rounds; 0: 20 */

    localization_candidate_filter_t candidate_filter;

} localization_robust_config_t;

/* Tukey loss, consensus on, 3 sigma inlier threshold. The caller must still
 * set propagation_speed_mps and timing_uncertainty_us. */
localization_robust_config_t localization_robust_default_config(void);


typedef enum
{
    LOCALIZATION_ROBUST_SOLVED = 0,
    LOCALIZATION_ROBUST_INSUFFICIENT_OBSERVATIONS,
    LOCALIZATION_ROBUST_NO_ALLOWED_CANDIDATE

} localization_robust_status_t;


typedef struct
{
    localization_robust_status_t status;
    bool planar;

    localization_point3d_t position;
    int64_t emission_time_us;
    double rms_residual_us;             /* over the inliers */
    double residual_us[LOCALIZATION_EXTENDED_MAX_SENSORS];

    size_t observations_used;           /* inliers */
    uint32_t inlier_mask;               /* bit i: observations[i] */
    uint32_t outlier_mask;

    bool consensus_run;
    bool consensus_ambiguous;
    size_t subsets_evaluated;
    size_t irls_iterations;

    /* inliers - (spatial unknowns + 1); <= 0 means the fit cannot confirm
     * itself. */
    int32_t degrees_of_freedom;

} localization_robust_result_t;


/*
 * observation_count in [1, LOCALIZATION_EXTENDED_MAX_SENSORS]. Returns
 * ESP_ERR_INVALID_ARG for invalid input, ESP_ERR_NO_MEM when the lattice
 * admission map cannot be allocated, ESP_OK otherwise (see result->status).
 */
esp_err_t localization_robust_solve(
    const localization_observation_t *observations,
    size_t observation_count,
    const localization_extended_bounds_t *bounds,
    const localization_robust_config_t *config,
    localization_robust_result_t *result
);
