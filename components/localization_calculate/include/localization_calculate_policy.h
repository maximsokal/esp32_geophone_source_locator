#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "localization_bayes.h"
#include "localization_calculate.h"
#include "localization_pick_record.h"
#include "localization_run_record.h"
#include "localization_comparison.h"
#include "localization_eikonal.h"
#include "localization_energy_baseline.h"
#include "localization_event_bridge.h"
#include "localization_psafuvs.h"
#include "localization_psp.h"
#include "localization_robust.h"
#include "localization_run_record.h"
#include "localization_semblance.h"
#include "localization_suea.h"
#include "localization_tdcas.h"
#include "localization_traveltime_inversion.h"
#include "localization_xcorr_tdoa.h"
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

/* Bound raw analysis frames by both duration and concurrent PSRAM use. */
size_t localization_calculate_analysis_window_frames(
    uint32_t sample_rate_hz,
    uint32_t max_analysis_seconds,
    uint64_t psram_budget_bytes,
    size_t largest_free_block_bytes,
    size_t safety_reserve_bytes,
    size_t bytes_per_frame,
    bool *limited_by_free_memory_out
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

/* Rescales every configured sample-count window from its recorded settings
 * rate to the session rate. Returns INVALID_ARG with a concise reason when
 * rounding makes a P or S STA/LTA configuration invalid. */
esp_err_t localization_calculate_rescale_sample_windows(
    localization_calculate_config_t *config,
    uint32_t session_sample_rate_hz,
    char *reason_out,
    size_t reason_size
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
    bool session_epoch_known;
    int64_t session_epoch_timestamp_us;
    uint64_t geometry_computed_at_ms;
    localization_dimensionality_t dimensionality;
    const char *propagation_model_name;
    const char *material;
    double propagation_speed_mps;
    double propagation_speed_uncertainty_mps;
    /* MAX-301: the normalized range copied into every run record (v5).
     * NULL (older callers, tests) records no range. */
    const localization_velocity_range_t *velocity_range;

} localization_calculate_run_context_t;

/* Add the TDOA iteration-cap outcome after the legacy event-record adapter. */
void localization_calculate_set_tdoa_terminal_status(
    const localization_event_result_t *event_result,
    uint8_t max_iterations,
    localization_run_record_t *record
);


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
    localization_failure_class_t not_attempted_failure_class,
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
 * MAX-301 S3: the slowest plausible speed used by the consistency gate.
 * An operator or preset range (source USER or PRESET) supplies it as
 * v_min. A range derived from the legacy scalar (SCALAR_DERIVED), an
 * unset range and an unknown source keep the pre-range rule
 * speed - uncertainty (or speed when that is not positive), bit for bit.
 * The gate never infers an upper speed bound from a small arrival
 * difference: a source equidistant from two sensors gives dt = 0 at any
 * speed, so fast-arrival inconsistency is left to the solvers' status.
 */
double localization_calculate_gate_min_speed_mps(const localization_calculate_config_t *config);

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



/* ---- Velocity-free methods: TDCAS, PSAFUVS, SUEA --------------------------
 * Each adapter writes the method's own estimated velocity (P, and S for
 * PSAFUVS) into estimated_*_velocity_mps and scores confidence from its
 * residual exactly like TDOA / traveltime inversion. A NULL result means
 * the method was not attempted; not_attempted_reason (PSAFUVS) must then
 * say why. */

void localization_calculate_tdcas_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_tdcas_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out,
    localization_velocity_status_t *velocity_status_out
);

void localization_calculate_psafuvs_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_psafuvs_result_t *result,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out,
    localization_velocity_status_t *velocity_status_out
);

void localization_calculate_suea_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_suea_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out,
    localization_velocity_status_t *velocity_status_out
);

/*
 * SUEA grid box from the traveltime search box: one search step wider in x
 * and y and one step higher, so the surface sensors (at the box top) and the
 * rays that bend around empty areas near the box edge stay inside the grid.
 * A planar search stays a single layer.
 */
void localization_calculate_suea_grid_bounds(
    const localization_traveltime_search_bounds_t *search,
    localization_point3d_t *min_out,
    localization_point3d_t *max_out
);

/* Redundancy of a fit with `unknowns` parameters to `observations`
 * independent arrivals: observations - unknowns (-1 when no observation).
 * TDCAS: N P picks, 5 unknowns (x, y, z, t0, v). PSAFUVS: 2N arrivals,
 * 6 unknowns (x, y, z, t0, vP, vS). */
int32_t localization_calculate_velocity_free_degrees_of_freedom(
    size_t observations,
    size_t unknowns
);


/* ---- Scan occupancy -> velocity-free methods (MAX-147/162) ---------------
 * Both callbacks take points in the SENSOR (localization) frame and map
 * them through the occupancy's registration, which the caller has already
 * checked with scan_occupancy_registration_usable(). */

/*
 * SUEA cell class of a sensor-frame point: scan-verified FREE air is
 * EMPTY (a wave cannot cross it), OCCUPIED surface is SOLID, UNKNOWN stays
 * UNKNOWN (passable, never assumed empty). OUTSIDE the scanned volume is
 * EMPTY only above the envelope top (open air over the rubble) and
 * UNKNOWN otherwise. `context` is a `const scan_occupancy_t *`.
 */
localization_suea_cell_t localization_calculate_scan_suea_cell(
    const localization_point3d_t *point,
    void *context
);


typedef struct
{
    const scan_occupancy_t *occupancy;

    /* Samples closer than this to the sensor end are ignored: a pod lies
     * ON the surface and registration puts it a few cm into the air as
     * often as into the rubble. */
    double sensor_clearance_m;

} localization_calculate_scan_ray_context_t;

/* Default sensor clearance: one and a half scan voxels. */
#define LOCALIZATION_CALCULATE_SCAN_RAY_CLEARANCE_VOXELS 1.5

/*
 * Straight-ray clearance for TDCAS / PSAFUVS: false when the segment
 * sensor -> candidate crosses scan-verified FREE space -- at least two
 * consecutive samples at half-voxel spacing, so a single noisy voxel does
 * not veto a ray a wave would simply pass. `context` is a
 * `const localization_calculate_scan_ray_context_t *`.
 */
bool localization_calculate_scan_ray_clear(
    const localization_point3d_t *sensor,
    const localization_point3d_t *candidate,
    void *context
);


/* ---- localization_extended: xcorr TDOA, robust, PSP, eikonal, Bayesian ----
 * Same conventions as the adapters above: a NULL result means "not
 * attempted" (a reason is then required where the signature takes one),
 * confidence comes from the residual, the caller applies redundancy. */

/* Their search box: the traveltime search box as-is. */
localization_extended_bounds_t localization_calculate_extended_bounds(
    const localization_traveltime_search_bounds_t *search
);

void localization_calculate_xcorr_tdoa_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_xcorr_tdoa_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);

/* Masks are channel masks (inliers used; outliers and missing picks
 * rejected). An ambiguous consensus (an inconsistency no pick can be
 * blamed for) caps the confidence like an unverified fit. */
void localization_calculate_robust_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_robust_result_t *result,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);

void localization_calculate_psp_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_psp_result_t *result,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);

/* method_name: "eikonal_fmm" or "eikonal_scan_voids". The velocity the fit
 * found (model velocity / slowness factor) is written only when
 * velocity_estimated (the factor was not fixed). */
void localization_calculate_eikonal_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_eikonal_result_t *result,
    const char *method_name,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    bool velocity_estimated,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);

/* method_name: "bayesian" or "bayesian_scan_voids". The MAP is the
 * position; the posterior region fills record_out->uncertainty; the
 * posterior-mean velocity is written when velocity_estimated. */
void localization_calculate_bayes_to_run_record(
    const localization_calculate_run_context_t *context,
    const localization_bayes_result_t *result,
    const char *method_name,
    const char *not_attempted_reason,
    localization_failure_class_t not_attempted_failure_class,
    bool velocity_estimated,
    uint32_t channel_mask_used,
    uint32_t channel_mask_rejected,
    uint32_t method_version,
    double timing_uncertainty_us,
    localization_run_record_t *record_out
);

/*
 * Standard deviation of an S - P interval for PSP: the P pick's own
 * sigma combined with the S pick's, which is the P sigma scaled by how
 * much weaker the S onset is (timing error grows as SNR falls):
 * sqrt(sP^2 + sS^2), sS = sP * max(1, p_snr / s_snr). A non-positive SNR
 * gives 10 x sP for the S pick.
 */
double localization_calculate_s_minus_p_sigma_us(
    double timing_uncertainty_us,
    double p_snr,
    double s_snr
);

/*
 * Eikonal velocity of a lattice node from the scan: 0 (impassable) only
 * when the node's whole cell is empty -- the node and the eight corners
 * of the cube +-sample_radius_m around it (the four in-plane corners when
 * `planar`) all classify EMPTY by localization_calculate_scan_suea_cell() --
 * and the model velocity otherwise. Blocking every node that merely
 * touches a void would widen it by half a cell on each side: a lattice
 * wave detours around blocked nodes, so a 0.3 m trench at 0.1 m cells
 * would act ~0.5 m wide. `context` is a
 * `const localization_calculate_scan_velocity_context_t *`.
 */
typedef struct
{
    const scan_occupancy_t *occupancy;
    double velocity_mps;
    double sample_radius_m;         /* half the eikonal cell */
    bool planar;

} localization_calculate_scan_velocity_context_t;

double localization_calculate_scan_eikonal_velocity(
    const localization_point3d_t *point,
    void *context
);


/*
 * Travel-time error, in microseconds, of a void edge discretized at
 * `cell_m`: a path bends at about two corners, each placed anywhere within
 * one cell (uniform, sd cell / sqrt 12), so cell / (v sqrt 6). 0 for
 * invalid input.
 */
double localization_calculate_discretization_error_us(double cell_m, double velocity_mps);


/* ---- Time budget ------------------------------------------------------
 * Each method is bracketed by budget_begin / budget_end. begin measures
 * the start and, for a skippable method past the deadline, refuses it;
 * end records the duration in the summary and, for a refused method,
 * marks its record unsolved with the reason. The clock is injected so
 * the policy is host-testable. */

typedef int64_t (*localization_calculate_clock_fn)(void);

typedef struct
{
    localization_calculate_clock_fn now_us;
    int64_t deadline_us;            /* 0 = none */
    localization_calculate_progress_fn progress;
    void *progress_context;
    localization_calculate_summary_t *summary;
    double max_horizontal_uncertainty_m;

    const char *method;
    int64_t started_us;
    bool allowed;

} localization_calculate_budget_t;

#define LOCALIZATION_CALCULATE_TIME_BUDGET_REASON \
    "not run: the calculation time budget was used up before this method"

void localization_calculate_budget_init(
    localization_calculate_budget_t *budget,
    localization_calculate_clock_fn now_us,
    int64_t deadline_us,
    localization_calculate_progress_fn progress,
    void *progress_context,
    localization_calculate_summary_t *summary,
    double max_horizontal_uncertainty_m
);

/* true when the method may run (always, unless skippable and past the
 * deadline). */
bool localization_calculate_budget_begin(
    localization_calculate_budget_t *budget,
    const char *method_name,
    bool skippable
);

/* record may be NULL (a step with no record of its own). */
void localization_calculate_budget_end(
    localization_calculate_budget_t *budget,
    localization_run_record_t *record
);

void localization_calculate_set_rejection(
    localization_run_record_t *record,
    const char *reason,
    localization_failure_class_t failure_class
);

/*
 * Contradicting methods (localization_comparison joint_conflict) are not a found source even though each of them solved:
 * the session recommendation becomes "record again" with the reason, never a success. The individual runs stay stored.
 */
void localization_calculate_mark_joint_conflict(localization_recommendation_t *recommendation);

/* Aggregates the newest run set and its matching pick record into the
 * session-level operator recommendation. */
void localization_calculate_recommendation(
    const localization_run_record_t *records,
    size_t record_count,
    const localization_pick_record_t *pick_records,
    size_t pick_record_count,
    localization_recommendation_t *recommendation_out
);
