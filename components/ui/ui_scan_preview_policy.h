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
