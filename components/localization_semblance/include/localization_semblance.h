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

    /* An approximate "event window center" shared by all channels (e.g.
     * derived from the earliest accepted geophone_events pick). This
     * backend never invents this reference itself -- without it, there is
     * no way to know which few hundred milliseconds of a long recording to
     * search. */
    size_t reference_sample_index;

    /* Each candidate's analysis window is
     * [reference_sample_index + predicted_shift - window_half_span_samples,
     *  reference_sample_index + predicted_shift + window_half_span_samples],
     * per channel, where predicted_shift is that channel's whole-sample
     * travel-time-to-the-candidate rounded to the nearest sample. */
    size_t window_half_span_samples;

    /* Coarse pass uses search_bounds.step_m; the refinement pass searches
     * +/- one coarse step around the coarse best point at
     * step_m / refine_divisor (>= 2). */
    uint32_t refine_divisor;

} localization_semblance_config_t;


typedef struct
{
    bool found;
    localization_point3d_t best_position;
    double best_semblance;      /* [0, 1]; 1.0 = perfectly coherent stack */
    double worst_semblance;     /* diagnostic: worst score seen in the search */
    size_t candidates_searched; /* diagnostic: total grid points evaluated (both passes) */

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
