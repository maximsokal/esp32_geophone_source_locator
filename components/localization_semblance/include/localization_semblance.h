#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"

/*
 * MAX-137: an independent source-location backend using cross-correlation/
 * traveltime alignment and a semblance (coherency) operator over candidate
 * positions -- the data-focusing / migration-style technique family
 * described in the ticket, implemented here as the standard array-
 * processing semblance measure (Neidell & Taner): for a candidate source
 * position, shift each channel's analysis window by that position's
 * predicted travel time, then measure how coherently the shifted windows
 * stack. Deliberately independent of localization_tdoa_solve() (the
 * existing Gauss-Newton solver) -- neither file references the other, so
 * a report/WebUI can show both methods' results side by side.
 *
 * This backend works directly on raw per-channel waveform windows, not on
 * single-sample arrival picks, because a coherency operator needs the
 * actual signal shape to measure -- callers should feed it seismic_dsp-
 * preprocessed (DC-removed) blocks, matching this project's established
 * pipeline boundary.
 *
 * "No assumption that rubble is homogeneous beyond the explicitly selected
 * propagation model": propagation_speed_mps is always caller-supplied
 * (via localization_config-style explicit input), never a default here.
 */

#define LOCALIZATION_SEMBLANCE_MAX_CHANNELS 8U

/* Each candidate costs channel_count x (2 * half_span + 1) samples. */
#define LOCALIZATION_SEMBLANCE_DEFAULT_MAX_COARSE_CANDIDATES 200000U


typedef struct
{
    const float *samples;
    size_t sample_count;
    uint32_t sample_rate_hz;
    localization_point3d_t sensor_position;

} localization_semblance_channel_t;


typedef struct
{
    localization_point3d_t min_corner;
    localization_point3d_t max_corner;
    double step_m;

    /* Set min_corner.z_m == max_corner.z_m for a 2D (planar) search --
     * the same code path naturally degenerates to a single-layer grid. */

} localization_semblance_search_bounds_t;


typedef struct
{
    double propagation_speed_mps;

    /* The EARLIEST observed arrival across all channels (e.g. the earliest
     * accepted geophone_events pick) -- an arrival time, not the unknown
     * emission time. This backend never invents this reference itself --
     * without it, there is no way to know which few hundred milliseconds of
     * a long recording to search. */
    size_t reference_sample_index;

    /* Each candidate's analysis window is
     * [reference_sample_index + (shift_i - min_j shift_j) - window_half_span_samples,
     *  reference_sample_index + (shift_i - min_j shift_j) + window_half_span_samples],
     * per channel, where shift_i is that channel's whole-sample
     * travel-time-to-the-candidate rounded to the nearest sample. Only the
     * differential delay is added (MAX-207): the reference already contains
     * the travel time to the nearest sensor. */
    size_t window_half_span_samples;

    /* Coarse pass uses search_bounds.step_m; the refinement pass searches
     * +/- one coarse step around the coarse best point at
     * step_m / refine_divisor (>= 2). */
    uint32_t refine_divisor;

    /* MAX-208 time budget: the coarse grid never exceeds this many
     * candidates (0 selects LOCALIZATION_SEMBLANCE_DEFAULT_MAX_COARSE_CANDIDATES);
     * a larger requested grid enlarges the coarse step by the smallest
     * integer factor that fits, reported as result.coarse_step_m_used. */
    size_t max_coarse_candidates;

    /* MAX-147: candidates the filter rejects are never scored. */
    localization_candidate_filter_t candidate_filter;

    /* Optional: per-channel travel times (sensor index = channel index)
     * replacing distance / propagation_speed_mps, e.g. eikonal fields
     * around a scan's voids. Candidates it cannot reach from every channel
     * are not scored. Zero-initialized keeps straight rays. */
    localization_travel_time_model_t travel_time_model;

} localization_semblance_config_t;


typedef struct
{
    bool found;
    localization_point3d_t best_position;
    double best_semblance;      /* [0, 1]; 1.0 = perfectly coherent stack */
    double worst_semblance;     /* diagnostic: worst score seen in the search */
    size_t candidates_searched; /* diagnostic: total grid points evaluated (both passes) */
    double coarse_step_m_used;  /* >= the requested step; larger only when over budget */

} localization_semblance_result_t;


/*
 * channel_count must be in [2, LOCALIZATION_SEMBLANCE_MAX_CHANNELS]. Every
 * channel must share the same sample_rate_hz and sample_count (this
 * project's AD7606 samples all channels simultaneously; mismatched buffers
 * from different windows are rejected rather than silently misaligned).
 */
esp_err_t localization_semblance_search(
    const localization_semblance_channel_t *channels,
    size_t channel_count,
    const localization_semblance_search_bounds_t *coarse_bounds,
    const localization_semblance_config_t *config,
    localization_semblance_result_t *result
);
