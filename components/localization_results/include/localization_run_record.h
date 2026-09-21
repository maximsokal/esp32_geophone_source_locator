#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization.h"
#include "localization_dimensionality_policy.h"

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
 * something this schema enforces by itself.
 */

#define LOCALIZATION_RUN_SCHEMA_VERSION 1U

#define LOCALIZATION_RUN_OBJECT_ID_SIZE   16U
#define LOCALIZATION_RUN_SESSION_ID_SIZE  16U
#define LOCALIZATION_RUN_METHOD_NAME_SIZE 32U
#define LOCALIZATION_RUN_REJECTION_REASON_SIZE 96U


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

    /* The propagation model actually used for this run (MAX-139 will grow
     * this into a fuller model + uncertainty; for now this is the same
     * scalar localization_config_t.propagation_speed_mps already requires
     * the caller to supply explicitly, never a hidden default). */
    double propagation_speed_mps;

    bool solved;

    /* Valid only when solved is true. */
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
