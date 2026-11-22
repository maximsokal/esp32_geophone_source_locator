#include "ui_scan_preview_policy.h"

#include <stdio.h>
#include <string.h>

#define IDENTITY_PREFIX "scan-crc32:"

bool ui_scan_preview_session_is_staged(const char *session_id)
{
    return session_id == NULL || session_id[0] == '\0';
}

bool ui_scan_preview_id_is_safe(const char *value, size_t capacity)
{
    if (value == NULL || value[0] == '\0' || capacity == 0U) return false;
    for (size_t i = 0U; i < capacity && value[i] != '\0'; ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return strnlen(value, capacity) < capacity;
}

bool ui_scan_preview_request_ids_valid(const char *object_id, size_t object_capacity, const char *session_id, size_t session_capacity)
{
    if (!ui_scan_preview_id_is_safe(object_id, object_capacity)) return false;
    return ui_scan_preview_session_is_staged(session_id) || ui_scan_preview_id_is_safe(session_id, session_capacity);
}

bool ui_scan_preview_staged_path(const char *objects_path, const char *object_id, const char *file_name, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0U) return false;
    out[0] = '\0';
    if (objects_path == NULL || object_id == NULL || file_name == NULL || file_name[0] == '\0') return false;
    const int written = snprintf(out, capacity, "%s/%s/staging/%s", objects_path, object_id, file_name);
    if (written <= 0 || (size_t)written >= capacity) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool ui_scan_preview_format_identity(uint32_t scan_crc, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0U) return false;
    out[0] = '\0';
    if (capacity < UI_SCAN_PREVIEW_IDENTITY_SIZE) return false;
    const int length = snprintf(out, capacity, IDENTITY_PREFIX "%08x", (unsigned)scan_crc);
    if (length != (int)(sizeof(IDENTITY_PREFIX) - 1U + 8U)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool ui_scan_preview_occupancy_matches_scan(const char *frame_id, uint32_t scan_crc)
{
    char expected[UI_SCAN_PREVIEW_IDENTITY_SIZE];
    if (frame_id == NULL) return false;
    if (!ui_scan_preview_format_identity(scan_crc, expected, sizeof(expected))) return false;
    return strcmp(frame_id, expected) == 0;
}

static bool has_suffix(const char *text, const char *suffix)
{
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

bool ui_scan_preview_method_is_scan_constrained(const char *method_name)
{
    if (method_name == NULL) return false;
    return has_suffix(method_name, "_scan_constrained") || has_suffix(method_name, "_voids");
}

ui_scan_preview_run_eligibility_t ui_scan_preview_run_eligibility(
    const ui_scan_preview_run_binding_t *run,
    const char *current_scan_identity)
{
    if (run == NULL) return UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN;
    const bool named_constrained = ui_scan_preview_method_is_scan_constrained(run->method_name);
    const bool recorded_constrained = run->scan_provenance_present && run->scan_constraint_applied;
    if (!named_constrained && !recorded_constrained) return UI_SCAN_PREVIEW_RUN_ELIGIBLE;

    if (!run->scan_provenance_present) return UI_SCAN_PREVIEW_RUN_LEGACY_CONSTRAINED;
    if (!run->scan_constraint_applied) return UI_SCAN_PREVIEW_RUN_CONSTRAINT_NOT_APPLIED;
    if (current_scan_identity == NULL || current_scan_identity[0] == '\0') return UI_SCAN_PREVIEW_RUN_SCAN_UNKNOWN;
    if (run->scan_frame_id == NULL || run->scan_frame_id[0] == '\0' ||
        strcmp(run->scan_frame_id, current_scan_identity) != 0) {
        return UI_SCAN_PREVIEW_RUN_STALE_SCAN;
    }
    return UI_SCAN_PREVIEW_RUN_ELIGIBLE;
}


ui_scan_preview_view_t ui_scan_preview_select_view(
    bool scan_present,
    bool scan_view_ready,
    size_t sensor_count,
    size_t source_count)
{
    if (scan_present) {
        return scan_view_ready ? UI_SCAN_PREVIEW_VIEW_SCAN : UI_SCAN_PREVIEW_VIEW_UNAVAILABLE;
    }
    return sensor_count > 0U || source_count > 0U
        ? UI_SCAN_PREVIEW_VIEW_SENSOR_ESTIMATES
        : UI_SCAN_PREVIEW_VIEW_UNAVAILABLE;
}
