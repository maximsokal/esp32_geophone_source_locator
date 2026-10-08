#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "localization_pick_record.h"
#include "localization_run_record.h"

/*
 * MAX-140: crash-safe persistence for a localization_run_record_t.
 * localization_run_record.h explicitly owns no file I/O or path
 * convention (its serialize/parse stays pure and host-testable); this is
 * the caller-side layer that decides WHERE a run is written and commits it
 * via the project's one existing atomic-write primitive
 * (storage_manager_atomic_write_text, temp-file + fsync + backup-swap),
 * following the same <objects>/<object_id>/sessions/<session_id>/ layout
 * every other per-session file (e.g. session.json) already uses.
 *
 * Every run gets its OWN file, named from kind + method_name +
 * computed_at_ms -- never a single fixed "latest result" path. That is
 * what makes "never overwrite the individual outputs" (MAX-138) and
 * "recalculating after adding a scan preserves the earlier unconstrained
 * result" (this ticket's own 2026-09-21 session-ownership clarification)
 * true by construction, with no caller discipline required.
 */

#define LOCALIZATION_RUN_STORE_PATH_MAX 160U
/* Maximal v1 picks JSON is 2730 bytes; include its terminating NUL. */
#define LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE 2731U
#define LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION 128U

/*
 * A generous cap: MAX-168's orchestrator (localization_calculate_run())
 * writes exactly one pick record per invocation, so this bounds how many
 * past "Calculate source" invocations one session's history can hold, not
 * how many channels a record itself carries (that is the fixed
 * LOCALIZATION_PICK_RECORD_CHANNEL_COUNT).
 */
#define LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION 64U

typedef enum
{
    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED = 1U << 0,
    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED = 1U << 1,
    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED = 1U << 2,
    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED = 1U << 3,
    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED = 1U << 4

} localization_run_store_partial_error_code_t;

typedef struct
{
    bool partial_error;
    size_t partial_error_count;
    /* Bit mask of localization_run_store_partial_error_code_t values. */
    uint32_t partial_error_codes;

} localization_run_store_list_status_t;


/*
 * Builds the path a given record would be written to/read from. Does not
 * touch the filesystem. Fails closed with ESP_ERR_INVALID_ARG on an
 * invalid object_id, a session_id not matching the project's canonical
 * "S######" form, or a method_name containing anything outside
 * [A-Za-z0-9_] -- method_name flows directly into the filename, and this
 * is the one place that actually matters for path safety.
 */
esp_err_t localization_run_store_build_path(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record,
    char *path_out,
    size_t path_out_size
);


/*
 * Serializes record and writes it crash-safely to its own path (see
 * localization_run_store_build_path). Returns whatever
 * localization_run_store_build_path or localization_run_record_to_json
 * returned on failure; otherwise the result of the underlying atomic
 * write.
 */
esp_err_t localization_run_store_write(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record
);


/*
 * Enumerates every run record already persisted for one session (see
 * localization_run_store.c for the full skip-on-error policy). Returns
 * ESP_ERR_NOT_FOUND if the session directory itself does not exist (no
 * recording under that id, or nothing has ever been written for it yet).
 */
esp_err_t localization_run_store_list(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out
);


/* Extended listing result. Valid records are retained when a matching file
 * cannot be read or parsed. NOT_FOUND means the session directory is absent
 * (ENOENT); other enumeration failures return an error and set partial_error. */
esp_err_t localization_run_store_list_ex(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
);


/*
 * MAX-148 (slice 3): the pick-record analog of the three functions above,
 * for localization_pick_record_t (components/localization_results). A
 * pick record is versioned by computed_at_ms exactly like a run record
 * (see localization_pick_record.h), but is not tied to any one method, so
 * its own path never includes a method_name.
 */
esp_err_t localization_run_store_build_picks_path(
    const char *object_id,
    const char *session_id,
    uint64_t computed_at_ms,
    char *path_out,
    size_t path_out_size
);


esp_err_t localization_run_store_write_picks(
    const char *object_id,
    const char *session_id,
    const localization_pick_record_t *record
);


esp_err_t localization_run_store_list_picks(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out
);


esp_err_t localization_run_store_list_picks_ex(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
);
