#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "localization_dimensionality_policy.h"

/*
 * MAX-165: the Record-button gate, independent of WebXR/3D scanning.
 *
 * Pure and stateless -- takes only the two counts the caller already has
 * from nrf_topology_service_get_leveling_summary() (assigned_count,
 * accepted_count), so this component has no ESP-IDF/topology dependency of
 * its own and is fully host-testable.
 *
 * Reuses localization_dimensionality_policy's 3-vs-4 threshold rather than
 * duplicating it, so the pre-recording gate and the post-recording
 * localization method-availability decision (MAX-170) always agree.
 *
 * Explicitly NOT covered here (needs a UI/session-state design decision,
 * not a pure function): letting the operator explicitly exclude one
 * specific non-vertical REMOTE from the participating set while keeping
 * the rest. This evaluates the aggregate "all assigned nodes are level"
 * case only.
 */


typedef enum
{
    /* Fewer than LOCALIZATION_DIMENSIONALITY_MIN_PLANAR_2D REMOTEs are
     * assigned. */
    RECORDING_GATE_BLOCKED_INSUFFICIENT_REMOTES = 0,

    /* Enough REMOTEs are assigned, but at least one has not reported
     * verticality ACCEPTED yet. */
    RECORDING_GATE_BLOCKED_NOT_LEVEL,

    /* Exactly 3 assigned, all level: recording is allowed, but source
     * localization for it will be planar/2D only. */
    RECORDING_GATE_ALLOWED_PLANAR_2D,

    /* 4 or more assigned, all level: recording is allowed and 3D-capable
     * localization may be attempted later (subject to that method's own
     * geometry/quality gates). */
    RECORDING_GATE_ALLOWED_3D_CAPABLE

} recording_gate_decision_t;


typedef struct
{
    recording_gate_decision_t decision;
    size_t assigned_count;
    size_t accepted_count;

    /* decision is always the diagnostic result. start_allowed also applies
     * CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING; when that option is
     * disabled, bypassed is true while decision still explains the gap. */
    bool start_allowed;
    bool bypassed;

    /* Meaningful only for the two ALLOWED_* decisions. */
    localization_dimensionality_t dimensionality;

} recording_gate_result_t;


recording_gate_result_t recording_gate_evaluate(
    size_t assigned_remote_count,
    size_t accepted_remote_count
);


/* Short, stable reason string for a BLOCKED decision. Returns "" (never
 * NULL) for either ALLOWED_* decision. */
const char *recording_gate_decision_reason(
    recording_gate_decision_t decision
);


/* Non-empty only for RECORDING_GATE_ALLOWED_PLANAR_2D: the exact warning
 * text this ticket requires ("only 3/4 remote geophones are active",
 * higher uncertainty, planar/2D-only, advice to connect one more). Returns
 * "" (never NULL) for every other decision. */
const char *recording_gate_planar_warning(
    recording_gate_decision_t decision
);


/* Stable reason token for a bypassed diagnostic decision; otherwise "". */
const char *recording_gate_bypass_reason(
    recording_gate_decision_t decision
);
