#pragma once

#include <stddef.h>

#include "esp_err.h"
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
