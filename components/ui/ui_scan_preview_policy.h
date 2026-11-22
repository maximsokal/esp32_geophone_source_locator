#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * MAX-415/MAX-414/MAX-412: pure decisions of the TFT scan preview, kept free
 * of LVGL, storage and RTOS so they can be tested on the host.
 *
 * Scan identity is the whole-file CRC32 of scan-v1.bin written as
 * "scan-crc32:%08x" (lowercase hex), the same string the occupancy artifact
 * stores as frame_id and the web UI calls occupancyFrameIdForScan.
 */

/* "scan-crc32:" (11) + 8 hex digits + NUL. */
#define UI_SCAN_PREVIEW_IDENTITY_SIZE 20U

typedef enum
{
    UI_SCAN_PREVIEW_VIEW_UNAVAILABLE = 0,
    UI_SCAN_PREVIEW_VIEW_SCAN,
    UI_SCAN_PREVIEW_VIEW_SENSOR_ESTIMATES,
} ui_scan_preview_view_t;

/*
 * Before a recording exists the phone saves the 3D scan under the object's staging directory and the workflow has no session id (issue #44
 * follow-up: the TFT 3D button at "ready to record" used to answer "invalid session id"). An EMPTY session id therefore means "the staged scan".
 */
bool ui_scan_preview_session_is_staged(const char *session_id);

/* An object or session id: non-empty, only letters, digits, '_' and '-', and shorter than `capacity` (so it cannot walk out of its directory). */
bool ui_scan_preview_id_is_safe(const char *value, size_t capacity);

/* The ids of a preview request: a safe object id and either a safe session id or the staged (empty) one. */
bool ui_scan_preview_request_ids_valid(const char *object_id, size_t object_capacity, const char *session_id, size_t session_capacity);

/* `<objects>/<object>/staging/<file>`; false (and an empty string) when it does not fit. */
bool ui_scan_preview_staged_path(const char *objects_path, const char *object_id, const char *file_name, char *out, size_t capacity);

/* Preserve scan-backed previews; use sensor-frame estimates only when no scan exists. */
ui_scan_preview_view_t ui_scan_preview_select_view(
    bool scan_present,
    bool scan_view_ready,
    size_t sensor_count,
    size_t source_count);

/* Writes the identity of `scan_crc`; false (and an empty string) when the
 * buffer is too small. */
bool ui_scan_preview_format_identity(uint32_t scan_crc, char *out, size_t capacity);

/* True only when `frame_id` is exactly the identity of `scan_crc`. A NULL,
 * empty, differently cased or longer string is not a match. */
bool ui_scan_preview_occupancy_matches_scan(const char *frame_id, uint32_t scan_crc);

/* Name-based classification shared with localization_calculate and the web
 * localizationOverlay: "<base>_scan_constrained" and any "_voids" method are
 * scan-constrained. There is no separate backend flag for legacy runs. */
bool ui_scan_preview_method_is_scan_constrained(const char *method_name);

typedef struct
{
    const char *method_name;
    /* localization_run_record_t schema 6 provenance (false for older runs). */
    bool scan_provenance_present;
    bool scan_constraint_applied;
    const char *scan_frame_id;
} ui_scan_preview_run_binding_t;

typedef enum
{
    UI_SCAN_PREVIEW_RUN_ELIGIBLE = 0,
    /* Constrained run without provenance: scan generation unknown (history). */
    UI_SCAN_PREVIEW_RUN_LEGACY_CONSTRAINED,
    /* Constrained-named run whose recorded provenance says no constraint was applied. */
    UI_SCAN_PREVIEW_RUN_CONSTRAINT_NOT_APPLIED,
    /* Constrained run bound to a different (replaced) scan. */
    UI_SCAN_PREVIEW_RUN_STALE_SCAN,
    /* Constrained run while the current scan identity could not be established. */
    UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN,
} ui_scan_preview_run_eligibility_t;

/*
 * Whether a run may be the CURRENT source. `current_scan_identity` is the
 * "scan-crc32:%08x" string of the validated scan, or NULL when unavailable.
 * Unconstrained runs are always eligible here (geometry generation is
 * enforced separately by the renderer). A joint fusion run follows the same
 * rule through its own provenance: it is constrained when it recorded an
 * applied scan constraint.
 */
ui_scan_preview_run_eligibility_t ui_scan_preview_run_eligibility(
    const ui_scan_preview_run_binding_t *run,
    const char *current_scan_identity);
