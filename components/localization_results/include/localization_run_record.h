#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_dimensionality_policy.h"
#include "localization_propagation_model.h"

/*
 * MAX-140: a versioned, backward-compatible record for one localization
 * run's result -- pure serialize/parse only. This component owns no file
 * I/O and no storage path convention; a caller (e.g. the future MAX-168
 * "Calculate source" handler) decides where a run's JSON text is written,
 * following whatever session-scoped layout MAX-166/167 settle on.
 *
 * "Individual method" and "joint fusion" results use the same record
 * shape (LOCALIZATION_RUN_KIND distinguishes them) so a session can persist
 * several of each without one silently overwriting another -- MAX-138's
 * "never overwrite the individual outputs" requirement is a caller-side
 * discipline (write each run under its own generation/version), not
 * something this schema enforces by itself. localization_run_store (a
 * separate component) is what actually assigns each run its own file.
 *
 * Schema v3 added confidence_score: v2 could only cleanly represent the
 * iterative Gauss-Newton TDOA solver's own result shape (iterations,
 * converged, emission_time_us), which the other backends in this epic
 * (semblance/coherency, statistical traveltime inversion, and the joint
 * fusion, MAX-136/137/138) have no equivalent of. Schema v4 adds the
 * producer-assigned failure class; schema v3 records without it still read
 * as LOCALIZATION_FAILURE_CLASS_NONE. Schema v6 adds the scan-generation
 * provenance for scan-constrained runs; older records remain history-only
 * when their scan generation cannot be established. Schema v7 adds
 * terminal solver status and optional recording-session epoch metadata.
*/

#define LOCALIZATION_RUN_SCHEMA_VERSION 7U
/* Schema v6: scan-generation provenance. */
#define LOCALIZATION_RUN_SCHEMA_VERSION_V6 6U
/* Schema v5: producer velocity range (MAX-301). */
#define LOCALIZATION_RUN_SCHEMA_VERSION_V5 5U
/* Schema v4: producer failure class, no velocity range. */
#define LOCALIZATION_RUN_SCHEMA_VERSION_V4 4U
#define LOCALIZATION_RUN_SCHEMA_VERSION_LEGACY 3U
#define LOCALIZATION_RUN_SCAN_FRAME_ID_SIZE 32U

#define LOCALIZATION_RUN_VELOCITY_SOURCE_UNKNOWN        0U
#define LOCALIZATION_RUN_VELOCITY_SOURCE_PRESET         1U
#define LOCALIZATION_RUN_VELOCITY_SOURCE_USER           2U
#define LOCALIZATION_RUN_VELOCITY_SOURCE_SCALAR_DERIVED 3U
#define LOCALIZATION_RUN_VELOCITY_SOURCE_MEASURED       4U

#define LOCALIZATION_RUN_VELOCITY_STATUS_UNKNOWN        0U
#define LOCALIZATION_RUN_VELOCITY_STATUS_NOT_APPLICABLE 1U
#define LOCALIZATION_RUN_VELOCITY_STATUS_OK             2U
#define LOCALIZATION_RUN_VELOCITY_STATUS_AT_BOUND       3U
#define LOCALIZATION_RUN_VELOCITY_STATUS_INCONSISTENT   4U

#define LOCALIZATION_RUN_OBJECT_ID_SIZE   16U
#define LOCALIZATION_RUN_SESSION_ID_SIZE  16U
#define LOCALIZATION_RUN_METHOD_NAME_SIZE 32U
/*
 * 128, not a smaller round number: localization_method_availability_reason()
 * (localization_dimensionality_policy.h)'s LOCALIZATION_METHOD_SKIPPED_
 * REQUIRES_3D string alone is 105 chars + NUL -- discovered by a real
 * truncation failure once a caller actually round-tripped that policy
 * string through this field instead of assuming it would fit.
 */
#define LOCALIZATION_RUN_REJECTION_REASON_SIZE 128U

#define LOCALIZATION_RUN_REJECTION_REASON_TIMESTAMP_NON_MONOTONIC "timestamp_non_monotonic"
#define LOCALIZATION_RUN_REJECTION_REASON_SESSION_GAP "session_gap"
#define LOCALIZATION_RUN_REJECTION_REASON_DUPLICATE_SENSOR_POSITION "duplicate_sensor_position"
#define LOCALIZATION_RUN_REJECTION_REASON_REFLECTED_TRANSFORM "reflected_transform"


typedef enum
{
    LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD = 0,
    LOCALIZATION_RUN_KIND_JOINT_FUSION

} localization_run_kind_t;


typedef enum
{
    LOCALIZATION_RUN_TERMINAL_STATUS_NONE = 0,
    LOCALIZATION_RUN_TERMINAL_STATUS_ITERATION_LIMIT,
    LOCALIZATION_RUN_TERMINAL_STATUS_TIME_BUDGET_SKIPPED,
    LOCALIZATION_RUN_TERMINAL_STATUS_TIME_BUDGET_HIT,
    LOCALIZATION_RUN_TERMINAL_STATUS_CELL_BUDGET,
    LOCALIZATION_RUN_TERMINAL_STATUS_RESOLUTION_LIMITED

} localization_run_terminal_status_t;


/*
 * Position uncertainty of a run that has one (the Bayesian methods): the
 * posterior's per-axis standard deviations and the smallest region
 * holding credible_level of the probability -- its volume (3D search) or
 * area (planar search) and bounding box. credible_level == 0 means "not
 * computed" (every other method, or a record written before the field).
 */
typedef struct
{
    localization_point3d_t std_m;
    double credible_level;
    double credible_volume_m3;
    double credible_area_m2;
    localization_point3d_t credible_min;
    localization_point3d_t credible_max;

    /* >= 1: the factor every sigma was scaled by because the fit's chi^2
     * showed them too small (1 = the configured sigmas explained the data;
     * absent in older records, read as 1). */
    double sigma_inflation;

} localization_run_uncertainty_t;


typedef struct
{
    uint32_t schema_version;

    char object_id[LOCALIZATION_RUN_OBJECT_ID_SIZE];
    char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE];
    uint64_t computed_at_ms;

    localization_run_kind_t kind;
    char method_name[LOCALIZATION_RUN_METHOD_NAME_SIZE];
    uint32_t method_version;

    /* Which of this session's channels contributed, and at what
     * dimensionality (MAX-135/MAX-170) -- lets a reader see *why* a run is
     * 2D-only or absent without re-deriving it from raw picks. */
    uint32_t channel_mask_used;
    uint32_t channel_mask_rejected;
    localization_dimensionality_t dimensionality;

    /* Ties this run to the exact frozen per-session geometry snapshot
     * (MAX-166), not whatever the object's live topology is now. 0 means
     * "not available" rather than "computed at epoch". */
    uint64_t geometry_computed_at_ms;

    /* Schema v6 scan provenance. scan_provenance_present distinguishes old
     * JSON records whose missing fields mean "unknown" from v6 records that
     * explicitly declare a constraint was or was not applied. Unconstrained
     * records use false, an empty frame id, and zero timestamps. */
    bool scan_provenance_present;
    bool scan_constraint_applied;
    char scan_frame_id[LOCALIZATION_RUN_SCAN_FRAME_ID_SIZE];
    uint64_t occupancy_created_at_ms;
    uint64_t registration_geometry_computed_at_ms;

    /* The propagation model actually used for this run (MAX-139). Name is
     * empty when the caller did not attach a named/calibrated model (still
     * a caller-supplied scalar via localization_config_t.propagation_speed_mps,
     * never a hidden default). uncertainty_mps is 0 when unknown, matching
     * localization_propagation_model_t's own "0 means unknown" convention. */
    char propagation_model_name[LOCALIZATION_PROPAGATION_MODEL_NAME_SIZE];
    /* Optional selected material ID; empty when the material was not set. */
    char material[24];
    double propagation_speed_mps;
    double propagation_speed_uncertainty_mps;

    bool solved;

    /* Candidate availability and solver completion are separate outcomes. */
    localization_run_terminal_status_t terminal_status;

    /* Absolute timestamp for recording sample zero. When unknown, this bool
     * is false and the timestamp value is ignored; zero is a valid epoch. */
    bool session_epoch_known;
    int64_t session_epoch_timestamp_us;

    /* The operator action class attached by the method that produced a
     * rejection. NONE is also the backward-compatible value for old JSON. */
    localization_failure_class_t failure_class;
    bool uncertainty_too_high;

    /*
     * A generic, method-defined [0, 1] confidence/focus score -- 0 means
     * "not computed", not "confirmed zero confidence". This is the one
     * field every method in the MAX-136/137/138 family can populate with
     * its own native quality measure without pretending to be an
     * iterative solver: TDOA / traveltime inversion use their own
     * Gaussian residual-based score, semblance uses best_semblance
     * directly, and localization_comparison's own `quality` (MAX-138)
     * uses exactly this same convention, so a joint-fusion run's
     * confidence_score is literally its joint_total_quality-derived
     * weight. Always present (even when solved is false) so a caller
     * never needs to special-case reading it.
     */
    double confidence_score;

    /*
     * MAX-212: statistical redundancy of a pick-based fit -- independent
     * observations beyond the unknowns (differential arrivals minus spatial
     * unknowns). 0 means an exactly-determined fit: its residual is ~0 by
     * construction and cannot confirm the position, so such a result is
     * "unverified". -1 means not applicable (semblance, energy, joint) or
     * unknown (a record written before this field existed).
     */
    int32_t degrees_of_freedom;

    /*
     * Velocity-free methods (TDCAS, PSAFUVS, SUEA) estimate the event's own
     * propagation velocities instead of using propagation_speed_mps: the P
     * (or single-phase) and S velocities they found, in m/s. 0 means "not
     * estimated" (every other method, or a record written before these
     * optional fields existed). Serialized only for a solved run.
     */
    double estimated_p_velocity_mps;
    double estimated_s_velocity_mps;

    /*
     * MAX-301 (schema v5): the propagation-velocity range the run used,
     * where it came from, and the method's verdict on it. Plain POD so this
     * component does not depend on localization_calculate. Values mirror
     * localization_velocity_range_source_t / localization_velocity_status_t:
     *   source: 0 UNKNOWN, 1 PRESET, 2 USER, 3 SCALAR_DERIVED, 4 MEASURED
     *   status: 0 UNKNOWN, 1 NOT_APPLICABLE, 2 OK, 3 AT_BOUND, 4 INCONSISTENT
     * All zero (UNKNOWN, no range) in records read from schema v3/v4.
     * vp_vs_min/max are 0 when no Vp/Vs bound was set.
     */
    float velocity_range_min_mps;
    float velocity_range_max_mps;
    float velocity_range_vp_vs_min;
    float velocity_range_vp_vs_max;
    uint8_t velocity_range_source;
    uint8_t velocity_status;

    /* Optional; serialized only for a solved run with credible_level > 0. */
    localization_run_uncertainty_t uncertainty;

    /* Valid only when solved is true. emission_time_us/iterations/
     * converged are meaningful only for the iterative Gauss-Newton TDOA
     * solver -- 0/false for every other method's solved run, not omitted,
     * so old readers never need method-specific parsing. */
    localization_point3d_t source_position;
    int64_t emission_time_us;
    double rms_residual_us;
    uint8_t iterations;
    bool converged;

    /* Valid only when solved is false: why this run has no XYZ candidate
     * (e.g. "insufficient channels", "solver rejected: degenerate
     * geometry"). Never left blank for an unsolved run. */
    char rejection_reason[LOCALIZATION_RUN_REJECTION_REASON_SIZE];

    /* Present when localization analyzed a truncated prefix of a session. */
    bool analysis_window_truncated;
    uint64_t analysis_window_start_index;
    uint64_t analysis_window_frames;
    char analysis_window_reason[16];

} localization_run_record_t;


/*
 * Serializes to a newly malloc'd, NUL-terminated JSON string in *json_out
 * (caller frees with free()). Returns ESP_ERR_INVALID_ARG for NULL
 * pointers, an unsolved record with an empty rejection_reason, or a solved
 * record with a non-finite source_position/residual.
 */
esp_err_t localization_run_record_to_json(
    const localization_run_record_t *record,
    char **json_out
);


/*
 * Parses a JSON string produced by localization_run_record_to_json() (or
 * anything satisfying the same schema). Rejects an unknown/future
 * schema_version and any structurally invalid document rather than
 * guessing at partial data -- an old reader must not silently misinterpret
 * a newer record. Returns ESP_ERR_INVALID_RESPONSE for malformed/invalid
 * JSON, ESP_ERR_NOT_FOUND for a recognized-but-unsupported schema_version.
 */
esp_err_t localization_run_record_from_json(
    const char *json,
    localization_run_record_t *record
);

/*
 * MAX-301 decision C: may this run be picked as "the" result? A solved run
 * whose method found the arrivals inconsistent with the velocity range is
 * kept and listed with a warning, but never selected as primary. Every
 * primary-result picker (TFT, WebUI, report) applies the same rule.
 */
static inline bool localization_run_record_is_primary_candidate(
    const localization_run_record_t *record
)
{
    return record != NULL && record->solved &&
        record->velocity_status != LOCALIZATION_RUN_VELOCITY_STATUS_INCONSISTENT;
}
