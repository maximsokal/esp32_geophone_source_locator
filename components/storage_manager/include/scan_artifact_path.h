#pragma once

/*
 * Where a session's scan artifacts (scan-v1.bin, occupancy-v1.bin, preview-v1.bin, registration-v1.json) are.
 *
 * One 3D scan serves every recording made with the same sensor placement, so the files are stored once, under
 *   <objects>/<object_id>/scans/<scan_id>/<file>          (scan_id: "SC" and six digits)
 * and a session that uses it carries only a one-line reference, <session>/scan-ref, holding the scan_id. FAT has no links
 * and a copy of an 8 MB scan to the SD card takes minutes, so a reference is the only way to give a second recording the
 * same voxels.
 *
 * A file inside the session itself always wins: sessions recorded before this scheme keep their own copy, and artifacts
 * made for one session (occupancy derived from the registration of its tags) stay with it. Only when the session has no
 * such file does the reference lead to the shared scan.
 *
 * These functions touch the file system: call them inside a storage operation, like every other access to the card.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SCAN_ARTIFACT_REF_FILE "scan-ref"
#define SCAN_ARTIFACT_ID_SIZE 9U /* "SC000001" and the terminator */

static inline bool scan_artifact_id_valid(const char *scan_id)
{
    if (scan_id == NULL || strlen(scan_id) != SCAN_ARTIFACT_ID_SIZE - 1U || scan_id[0] != 'S' || scan_id[1] != 'C') {
        return false;
    }
    for (size_t i = 2U; i < SCAN_ARTIFACT_ID_SIZE - 1U; ++i) {
        if (scan_id[i] < '0' || scan_id[i] > '9') return false;
    }
    return true;
}

static inline int scan_artifact_session_path(
    const char *objects_path, const char *object_id, const char *session_id, const char *file_name,
    char *out, size_t out_size)
{
    const int written = file_name == NULL || file_name[0] == '\0'
        ? snprintf(out, out_size, "%s/%s/sessions/%s", objects_path, object_id, session_id)
        : snprintf(out, out_size, "%s/%s/sessions/%s/%s", objects_path, object_id, session_id, file_name);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static inline int scan_artifact_shared_path(
    const char *objects_path, const char *object_id, const char *scan_id, const char *file_name,
    char *out, size_t out_size)
{
    const int written = file_name == NULL || file_name[0] == '\0'
        ? snprintf(out, out_size, "%s/%s/scans/%s", objects_path, object_id, scan_id)
        : snprintf(out, out_size, "%s/%s/scans/%s/%s", objects_path, object_id, scan_id, file_name);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static inline bool scan_artifact_file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    (void)fclose(file);
    return true;
}

/* The scan the session refers to; false when it has no reference or the reference is not a scan id. */
static inline bool scan_artifact_read_ref(
    const char *objects_path, const char *object_id, const char *session_id, char scan_id[SCAN_ARTIFACT_ID_SIZE])
{
    char path[256];
    if (scan_artifact_session_path(objects_path, object_id, session_id, SCAN_ARTIFACT_REF_FILE, path, sizeof(path)) != 0) {
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char text[SCAN_ARTIFACT_ID_SIZE + 2U] = {0};
    const size_t count = fread(text, 1U, sizeof(text) - 1U, file);
    (void)fclose(file);
    size_t length = count;
    while (length > 0U && (text[length - 1U] == '\n' || text[length - 1U] == '\r' || text[length - 1U] == ' ')) --length;
    if (length != SCAN_ARTIFACT_ID_SIZE - 1U) return false;
    memcpy(scan_id, text, SCAN_ARTIFACT_ID_SIZE - 1U);
    scan_id[SCAN_ARTIFACT_ID_SIZE - 1U] = '\0';
    return scan_artifact_id_valid(scan_id);
}

/*
 * The path to read `file_name` of this session from: its own file, else the shared scan's file, else the session's own
 * path (so that "not found" is reported exactly as before). Returns 0, or -1 when a path does not fit `out`.
 */
static inline int scan_artifact_resolve(
    const char *objects_path, const char *object_id, const char *session_id, const char *file_name,
    char *out, size_t out_size)
{
    if (scan_artifact_session_path(objects_path, object_id, session_id, file_name, out, out_size) != 0) return -1;
    if (scan_artifact_file_exists(out)) return 0;
    char scan_id[SCAN_ARTIFACT_ID_SIZE];
    if (!scan_artifact_read_ref(objects_path, object_id, session_id, scan_id)) return 0;
    char shared[256];
    if (scan_artifact_shared_path(objects_path, object_id, scan_id, file_name, shared, sizeof(shared)) != 0) return -1;
    if (!scan_artifact_file_exists(shared)) return 0;
    if (strlen(shared) >= out_size) return -1;
    memcpy(out, shared, strlen(shared) + 1U);
    return 0;
}
