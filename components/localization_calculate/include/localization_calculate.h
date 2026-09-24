#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "geophone_events.h"
#include "localization_propagation_model.h"

/*
 * MAX-168: "Calculate source" for one finalized, saved recording/session.
 * Composes every localization backend this diploma project has built
 * (localization_event_bridge's TDOA solver, MAX-136 traveltime inversion,
 * MAX-137 semblance, MAX-138 energy baseline, MAX-138 joint fusion) against
 * that session's own frozen geometry (MAX-166) and raw samples -- never the
 * device's current live topology -- and persists each method's own result
 * separately via localization_run_store, exactly as MAX-168 requires.
 *
 * Per this project's standing rule (MAX-132: "No production implementation
 * should start from guessed filter bands or propagation velocity"), nothing
 * here invents propagation speed, pick-detection thresholds, or timing
 * uncertainty -- every physically-measured parameter is required, explicit
 * caller input. The one thing this DOES derive automatically is each grid-
 * search backend's search box/resolution, because that is a numerical
 * search-strategy choice computed from the session's own solved sensor
 * positions, not a physical measurement -- the same distinction
 * localization_default_config() already draws between defaultable solver
 * knobs (max_iterations, convergence_m, damping) and the one field it
 * deliberately leaves zeroed (propagation_speed_mps).
 *
 * Velocity-free methods (TDCAS, PSAFUVS, SUEA; components/
 * localization_velocity_free) run beside them on the same picks: they
 * estimate the event's own velocity instead of trusting
 * propagation_speed_mps (SUEA takes it only as a prior with the caller's
 * uncertainty). PSAFUVS additionally needs S picks (s_pick_enabled).
 *
 * Scan constraint (MAX-147): when THIS session saved a scan occupancy whose
 * registration matches its frozen geometry, constrained variants are
 * persisted beside -- never fused into -- the unconstrained results:
 * candidate filtering (traveltime, semblance, TDCAS, PSAFUVS), straight-ray
 * void screening (TDCAS, PSAFUVS drop sensors/combinations whose ray
 * crosses scan-verified FREE space) and SUEA's travel paths around the
 * scanned empty areas (suea_scan_voids). UNKNOWN is never treated as empty.
 */


typedef struct
{
    /* Required: no universal propagation speed exists for heterogeneous
     * rubble/structures (localization.h's own words). > 0. */
    double propagation_speed_mps;

    /* Required (>= 0; 0 means "unknown", matching
     * localization_propagation_model_t's own convention). */
    double propagation_speed_uncertainty_mps;

    /* Optional caller label for the model that produced the two fields
     * above (e.g. "tap-calibrated 2026-09-20"); may be empty. */
    char propagation_model_name[LOCALIZATION_PROPAGATION_MODEL_NAME_SIZE];

    /* Required (> 0): assumed 1-sigma arrival-time pick noise in
     * microseconds, e.g. from MAX-142's empirical timing-jitter finding.
     * Feeds localization_traveltime_search() and this run's own
     * residual->quality mapping (see localization_calculate_policy.h). */
    double timing_uncertainty_us;

    /* Required: every field must be filled in by the caller from
     * measured/reviewed values -- geophone_events.h itself refuses to
     * default any of these. */
    geophone_channel_pick_config_t pick_config;

    /* Required (> 0): half-width, in samples, of the coherency window the
     * semblance backend stacks around its reference sample -- an event-
     * duration characteristic, not a numerical search-strategy knob, so
     * this stays explicit caller input like the pick config above. */
    size_t semblance_window_half_span_samples;

    /* Optional (MAX-211): how far below the lowest sensor a 3D search may
     * place the source, in meters. <= 0 selects the geometry-derived default
     * (see localization_calculate_search_bounds()). The search never goes
     * above the highest sensor. */
    double max_source_depth_m;

    /* Optional (PSAFUVS): pick the S-wave onset after each accepted P pick.
     * false leaves PSAFUVS persisted as not attempted, with that reason;
     * when true every s_pick_config field must come from measured/reviewed
     * values, exactly like pick_config (MAX-132). */
    bool s_pick_enabled;
    geophone_secondary_pick_config_t s_pick_config;

} localization_calculate_config_t;


typedef struct
{
    size_t participating_channel_count;
    size_t methods_attempted;
    size_t methods_solved;
    size_t methods_persisted;

} localization_calculate_summary_t;


/*
 * Runs every available method against object_id/session_id's frozen
 * geometry and raw samples, persists each individual method's result plus
 * one joint-fusion result via localization_run_store, and reports a summary.
 *
 * Fails closed (no run store writes at all) if the session is not
 * finalized/readable, its frozen geometry has fewer than
 * LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D participating pods, or the
 * session's raw sample count exceeds this build's analysis buffer capacity.
 * A method that cannot run for a data reason (not enough accepted picks,
 * dimensionality too low for that specific method, a solver rejection) is
 * not a failure of this function -- it is persisted as its own unsolved run
 * record with a reason, per the ticket's "preserve unavailable/rejected
 * methods with a reason" requirement.
 */
esp_err_t localization_calculate_run(
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint64_t computed_at_ms,
    localization_calculate_summary_t *summary_out
);
