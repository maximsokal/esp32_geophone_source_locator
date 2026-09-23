#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization_comparison.h"
#include "localization_energy_baseline.h"
#include "localization_event_bridge.h"
#include "localization_run_record.h"
#include "localization_semblance.h"
#include "localization_traveltime_inversion.h"
#include "object_store.h"
#include "scan_occupancy.h"

/*
 * Pure policy/adapter layer for MAX-168: everything here is plain math over
 * caller-supplied structs, with no filesystem, sensor, or DSP access, so it
 * is fully host-testable independent of localization_calculate.c's
 * orchestration. See localization_calculate.h for the full design rationale.
 */


/*
 * Shared "quality from a microsecond residual" mapping -- the exact formula
 * localization_comparison.h itself suggests for both TDOA and traveltime
 * inversion: exp(-0.5 * (residual_us / timing_uncertainty_us)^2). Returns 0
 * for a non-finite/non-positive residual or a non-positive
 * timing_uncertainty_us rather than propagating NaN/Inf into a persisted
 * record.
 */
double localization_calculate_quality_from_residual_us(
    double residual_us,
    double timing_uncertainty_us
);


/*
 * Counts pods with computed == true and returns the matching channel_mask
 * (bit i set iff geometry->pods[i].computed), matching this project's
 * established channel_mask_used/rejected convention (pod index == AD7606
 * channel index, confirmed equal elsewhere in this codebase).
 */
size_t localization_calculate_participating_channels(
    const object_store_geometry_t *geometry,
    uint32_t *channel_mask_out
);


/*
 * Converts one pod's frozen millimeter geometry to the meter-based
 * localization_point3d_t every backend consumes. Caller must have already
 * checked pod->computed; z is passed through unmodified (already
 * MAIN-relative per the project's established geometry convention).
 */
localization_point3d_t localization_calculate_sensor_position(
    const object_store_pod_geometry_t *pod
);


/*
 * A single "this session's geometry was frozen as of" timestamp derived from
 * per-pod computed_at_ms (object_store_geometry_t has no session-level
 * timestamp of its own): the latest computed_at_ms among only the computed
 * pods, i.e. the moment every currently-valid pod position was already
 * available. 0 if geometry is NULL or no pod is computed.
 */
uint64_t localization_calculate_geometry_computed_at_ms(
    const object_store_geometry_t *geometry
);


/*
 * Converts a whole-sample arrival index to a microsecond arrival time via
 * the exact sample index and sample rate -- the same precision-preserving
 * conversion localization_event_bridge.c already uses for TDOA, reused here
 * so traveltime inversion's picks are built the identical way. Returns 0
 * for sample_rate_hz == 0 rather than dividing by zero.
 */
int64_t localization_calculate_arrival_time_us(
    size_t arrival_sample_index,
    uint32_t sample_rate_hz
);


/*
 * Derives a grid-search box from the session's own solved sensor positions
 * -- a numerical search-strategy choice computed here, not a physical
 * measurement, unlike propagation speed (see localization_calculate.h).
 * Collapses to a single Z layer (min_corner.z_m == max_corner.z_m == the
 * mean sensor Z) when dimensionality is PLANAR_2D, matching both grid-
 * search backends' own documented 2D convention. Horizontal margin and step
 * are fixed, documented constants (localization_calculate_policy.c).
 *
 * MAX-211 (3D): geophones lie ON the surface and the source is buried, so
 * the box never extends above the highest sensor (a near-planar surface
 * array cannot distinguish a source below it from its mirror image above
 * it; only the physical one is searched). It extends max_source_depth_m
 * below the lowest sensor; max_source_depth_m <= 0 (or non-finite) selects
 * the default: the horizontal margin (half the array diagonal, >= 1 m).
 */
void localization_calculate_search_bounds(
    const localization_point3d_t *positions,
    size_t position_count,
    localization_dimensionality_t dimensionality,
    double max_source_depth_m,
    localization_semblance_search_bounds_t *semblance_bounds_out,
    localization_traveltime_search_bounds_t *traveltime_bounds_out
);


/* Fields every adapter below fills identically; method-specific fields are
 * each adapter's own job. */
typedef struct
{
    const char *object_id;
    const char *session_id;
    uint64_t computed_at_ms;
    uint64_t geometry_computed_at_ms;
    localization_dimensionality_t dimensionality;
    const char *propagation_model_name;
    double propagation_speed_mps;
    double propagation_speed_uncertainty_mps;

} localization_calculate_run_context_t;


void localization_calculate_traveltime_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_traveltime_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);


/*
 * result may be NULL when localization_semblance_search() was never called
 * at all (no accepted arrival pick anywhere to anchor its search window --
 * a precondition external to the library call itself); not_attempted_reason
 * must then be a non-empty, stable string. When result is non-NULL,
 * not_attempted_reason is ignored.
 */
void localization_calculate_semblance_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_semblance_result_t *result,
    const char *not_attempted_reason,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
);


void localization_calculate_energy_baseline_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_energy_baseline_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
);


void localization_calculate_joint_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_comparison_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    localization_run_record_t *record_out
);


/*
 * Maps one already-persisted individual-method run record into the
 * [0,1]-quality comparison entry localization_comparison_fuse() consumes,
 * using each method's own quality field verbatim (every individual adapter
 * above already wrote confidence_score using the exact per-method mapping
 * localization_comparison.h itself suggests) -- this function performs no
 * additional scoring of its own, only the struct/field translation.
 */
void localization_calculate_run_record_to_comparison_method(
    const localization_run_record_t *record,
    localization_comparison_method_result_t *method_out
);


/*
 * MAX-212: the most a solved-but-unverifiable (degrees_of_freedom <= 0)
 * residual-based fit may claim. With zero redundancy a wrong pick set fits
 * exactly as well as a right one, so the residual carries no evidence
 * either way; 0.5 is "no better than a coin flip", never "confirmed".
 */
#define LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP 0.5

/* Evidence group shared by every method fitted to the same arrival picks. */
#define LOCALIZATION_CALCULATE_PICK_EVIDENCE_GROUP 1U

/*
 * Redundancy of a differential-arrival fit: (picks_used - 1) independent
 * differences minus the spatial unknowns (2 for a planar search, 3 for 3D).
 * Returns -1 when fewer than two picks exist.
 */
int32_t localization_calculate_degrees_of_freedom(
    size_t picks_used,
    bool planar
);

/*
 * Stores `degrees_of_freedom` on a solved pick-based record and, when it is
 * <= 0, caps confidence_score at LOCALIZATION_CALCULATE_UNVERIFIED_CONFIDENCE_CAP.
 */
void localization_calculate_apply_redundancy(
    localization_run_record_t *record,
    int32_t degrees_of_freedom
);

/*
 * MAX-212: the analysis window for event energy, instead of the whole
 * recording (which is dominated by background noise): from the earliest
 * accepted pick to that pick + signal_window_samples + the largest moveout
 * across the array (max_sensor_distance_m / propagation_speed_mps), clamped
 * to [0, frame_count). Returns false (window unset) for invalid inputs or a
 * reference outside the recording.
 */
bool localization_calculate_event_window(
    size_t reference_sample_index,
    size_t frame_count,
    size_t signal_window_samples,
    double max_sensor_distance_m,
    double propagation_speed_mps,
    uint32_t sample_rate_hz,
    size_t *start_out,
    size_t *end_out
);


/* MAX-209: a pick's sub-sample onset position, in samples. */
double localization_calculate_pick_sample_position(
    const geophone_channel_pick_t *pick
);

/* Sub-sample-aware variant of localization_calculate_arrival_time_us(). */
int64_t localization_calculate_arrival_time_us_at(
    double sample_position,
    uint32_t sample_rate_hz
);

/*
 * MAX-209: physical consistency gate across channels. Two picks i, j are
 * compatible when |t_i - t_j| <= distance(i, j) / v_min + tolerance_s, with
 * v_min = propagation_speed_mps - propagation_speed_uncertainty_mps (or
 * propagation_speed_mps when that is not positive): no single wave can
 * reach two sensors further apart in time than their separation allows.
 * Returns the largest subset of `candidate_mask` whose members are all
 * pairwise compatible (ties: the subset with the smallest arrival-time
 * span, then the lowest mask value). Channels outside the returned mask
 * should be rejected as GEOPHONE_PICK_REJECTED_INCONSISTENT.
 */
uint32_t localization_calculate_consistent_pick_mask(
    const double *arrival_time_s,
    const localization_point3d_t *positions,
    size_t channel_count,
    uint32_t candidate_mask,
    double propagation_speed_mps,
    double propagation_speed_uncertainty_mps,
    double tolerance_s
);

/*
 * Minimum normalized correlation for a cross-correlation delay to replace
 * the per-channel pick difference (MAX-209). Below it the two windows are
 * not similar enough for the correlation peak to mean "same wavelet".
 */
#define LOCALIZATION_CALCULATE_XCORR_MIN_CORRELATION 0.7

/* Largest change, in samples, cross-correlation may make to the picked
 * relative delay; beyond it the correlation is assumed to have locked onto
 * a neighbouring cycle of the wavelet. */
#define LOCALIZATION_CALCULATE_XCORR_MAX_ADJUST_SAMPLES 2.0

/*
 * MAX-209: refines the relative delay of `channel` behind `reference`
 * (both full-length, same sample_count) by cross-correlating equal windows
 * [start, start + window + max_lag) with start = reference_pick -
 * window / 2 (clamped). Succeeds only when the correlation peak reaches
 * LOCALIZATION_CALCULATE_XCORR_MIN_CORRELATION and the sub-sample delay lies
 * within max_adjust_samples of pick_delay_samples (the independent pick
 * difference) -- a larger disagreement means the correlation locked onto a
 * different cycle, and the picks are kept.
 */
bool localization_calculate_xcorr_relative_delay(
    const float *reference,
    const float *channel,
    size_t sample_count,
    size_t reference_pick_index,
    double pick_delay_samples,
    size_t window_samples,
    size_t max_lag_samples,
    double max_adjust_samples,
    double *delay_samples_out
);


/*
 * MAX-147: candidate filter for the grid-search backends. `context` is a
 * `const scan_occupancy_t *` whose registration the caller has already
 * checked with scan_occupancy_registration_usable(). The candidate (sensor
 * frame) is transformed into the scan frame and admitted only in UNKNOWN or
 * OCCUPIED cells: scan-verified FREE air and space outside the scanned
 * candidate volume can never hold the source. UNKNOWN is never treated as
 * empty.
 */
bool localization_calculate_scan_candidate_allowed(
    const localization_point3d_t *candidate,
    void *context
);

