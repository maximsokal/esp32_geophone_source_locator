#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_extended.h"

/*
 * Network cross-correlation TDOA. Instead of one onset pick per channel,
 * every channel pair (i, j) is cross-correlated around the event and the
 * correlation peak gives the delay d_ij = t_j - t_i directly (sub-sample,
 * by parabolic interpolation). N channels give N(N-1)/2 delays for N-1
 * independent ones -- 6 for the current 4 pods -- so the redundancy is
 * used twice:
 *
 *   closure   every triangle must satisfy d_ij + d_jk = d_ik; the least-
 *             squares network adjustment reports its misfit and the
 *             adjusted per-channel delays;
 *   location  the source is fitted to all pair delays at once with a
 *             robust (Huber by default) loss, each pair weighted by its
 *             correlation coefficient, so a pair that locked onto the
 *             wrong cycle is down-weighted and reported.
 *
 * Model: d_ij = (|x - s_j| - |x - s_i|) / v with the caller's velocity.
 * The simultaneous AD7606 sampling is what makes waveform delays between
 * channels meaningful at all: there is no multiplexer skew to remove.
 */

#define LOCALIZATION_XCORR_MAX_PAIRS \
    (LOCALIZATION_EXTENDED_MAX_SENSORS * (LOCALIZATION_EXTENDED_MAX_SENSORS - 1U) / 2U)


typedef struct
{
    const float *samples;
    size_t sample_count;
    uint32_t sample_rate_hz;
    localization_point3d_t sensor_position;

    /* The channel's onset pick, in (sub-)samples, when it has one. It only
     * anchors the correlation window; the delay comes from the waveforms. */
    bool pick_valid;
    double pick_sample;

} localization_xcorr_channel_t;


typedef struct
{
    size_t first;              /* channel i */
    size_t second;             /* channel j */
    double delay_s;            /* t_j - t_i */
    double correlation;        /* normalized peak, (0, 1] */
    double sigma_s;            /* from the correlation (see config) */

} localization_xcorr_pair_t;


typedef struct
{
    double propagation_speed_mps;

    /* The delay standard deviation of a pair correlating exactly at
     * min_correlation; better-correlated pairs get sigma scaled by
     * sqrt(g(min) / g(c)), g(c) = c^2 / (1 - c^2) (c capped at 0.995). */
    double timing_uncertainty_us;

    size_t window_samples;     /* correlation window length (>= 4) */
    size_t max_lag_samples;    /* largest |delay| searched (physical bound) */
    double min_correlation;    /* <= 0: 0.7 */

    /* When both channels have picks, only delays within this many samples
     * of the pick difference are searched; <= 0 searches +-max_lag. */
    double max_pick_disagreement_samples;

    localization_extended_loss_t loss;  /* default (0): Huber */
    double tuning_k;                    /* <= 0: the loss's default */
    size_t max_lattice_points;          /* seeding lattice; 0: 8000 */

    localization_candidate_filter_t candidate_filter;

} localization_xcorr_tdoa_config_t;


typedef enum
{
    LOCALIZATION_XCORR_TDOA_SOLVED = 0,
    LOCALIZATION_XCORR_TDOA_NO_ANCHOR,
    LOCALIZATION_XCORR_TDOA_INSUFFICIENT_PAIRS,
    LOCALIZATION_XCORR_TDOA_NO_ALLOWED_CANDIDATE,
    LOCALIZATION_XCORR_TDOA_UNDERDETERMINED

} localization_xcorr_tdoa_status_t;


typedef struct
{
    localization_xcorr_tdoa_status_t status;
    bool planar;

    localization_point3d_t position;
    double rms_pair_residual_us;        /* over the pairs not flagged */
    double closure_rms_us;              /* network adjustment misfit */
    double mean_correlation;

    size_t pairs_total;                 /* N(N-1)/2 */
    size_t pairs_measured;              /* correlation >= min */
    size_t pairs_used;                  /* measured and not flagged */
    uint32_t pair_outlier_mask;         /* bit k: pairs[k] of the measured list */
    uint32_t channel_mask_used;         /* bit i: in at least one used pair */
    size_t channels_used;

    /* Network-adjusted arrival delays behind the earliest used channel
     * (0 for channels not used). */
    double relative_delay_us[LOCALIZATION_EXTENDED_MAX_SENSORS];

    /* (channels_used - 1) - spatial unknowns: the redundancy of the
     * independent delays. The closure adds pair-level redundancy on top. */
    int32_t degrees_of_freedom;
    double normal_rcond;

} localization_xcorr_tdoa_result_t;


/*
 * Measures every pair's delay. pairs_out must hold
 * LOCALIZATION_XCORR_MAX_PAIRS entries; only pairs reaching
 * min_correlation are written. ESP_ERR_INVALID_ARG for invalid input
 * (channel_count outside [2, 8], mismatched lengths or sample rates),
 * ESP_ERR_NOT_FOUND when no channel has a pick to anchor the windows.
 */
esp_err_t localization_xcorr_measure_pairs(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_pair_t *pairs_out,
    size_t *pair_count_out,
    size_t *pairs_total_out
);

/* Locates the source from measured pairs (sensor positions from
 * channels[]). */
esp_err_t localization_xcorr_tdoa_locate(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_xcorr_pair_t *pairs,
    size_t pair_count,
    const localization_extended_bounds_t *bounds,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_tdoa_result_t *result
);

/* measure_pairs + locate. */
esp_err_t localization_xcorr_tdoa_solve(
    const localization_xcorr_channel_t *channels,
    size_t channel_count,
    const localization_extended_bounds_t *bounds,
    const localization_xcorr_tdoa_config_t *config,
    localization_xcorr_tdoa_result_t *result
);
