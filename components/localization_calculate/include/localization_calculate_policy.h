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
