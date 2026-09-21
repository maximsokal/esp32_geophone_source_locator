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
 * fusion, MAX-136/137/138) have no equivalent of.
 */

#define LOCALIZATION_RUN_SCHEMA_VERSION 3U

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


typedef enum
{
    LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD = 0,
    LOCALIZATION_RUN_KIND_JOINT_FUSION

} localization_run_kind_t;


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

    /* The propagation model actually used for this run (MAX-139). Name is
     * empty when the caller did not attach a named/calibrated model (still
     * a caller-supplied scalar via localization_config_t.propagation_speed_mps,
     * never a hidden default). uncertainty_mps is 0 when unknown, matching
     * localization_propagation_model_t's own "0 means unknown" convention. */
    char propagation_model_name[LOCALIZATION_PROPAGATION_MODEL_NAME_SIZE];
    double propagation_speed_mps;
    double propagation_speed_uncertainty_mps;

    bool solved;

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
