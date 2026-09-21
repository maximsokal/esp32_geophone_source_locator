#include "object_recovery_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "seismic_recorder.h"
#include "storage_manager.h"


#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif


static const char *TAG = "OBJECT_RECOVERY";

#define OBJECT_RECOVERY_SCAN_FILE_NAME "scan-v1.bin"
#define OBJECT_RECOVERY_SCAN_PATH_SIZE 192U


static bool object_recovery_file_exists(const char *path)
{
    struct stat info;
    return path != NULL && stat(path, &info) == 0 && S_ISREG(info.st_mode);
}


/*
 * Session IDs are fixed-width, zero-padded "SNNNNNN" (seismic_recorder's own
 * convention), so lexicographic comparison is also numeric/chronological
 * comparison -- no separate numeric parse is needed to find the newest one.
 */
static const char *object_recovery_newest_session_id(
    const seismic_recorder_session_summary_t *sessions,
    size_t count
)
{
    const char *newest = NULL;
    for (size_t i = 0U; i < count; ++i) {
        if (newest == NULL || strcmp(sessions[i].session_id, newest) > 0) {
            newest = sessions[i].session_id;
        }
    }
    return newest;
}


/*
 * MAX-167: 3D scans used to be stored directly under the object
 * (<objects>/<object_id>/scan-v1.bin). A scan now belongs to one recording/
 * session, so any such legacy file found for an object is attached to that
 * object's newest recorded session (highest session_id) -- the closest
 * approximation to "the session it was probably captured for" available
 * without guessing further.
 *
 * An object with no sessions at all has nowhere sensible to attach the scan
 * to; it is left exactly where it is rather than deleted or guessed at. A
 * legacy file is also left in place if the destination session already has
 * its own scan (never overwritten) or if anything about the move fails.
 *
 * Always returns ESP_OK: a stuck legacy scan for one object must never block
 * the wider per-object recovery pass this runs inside, so every failure path
 * here is logged, not propagated.
 */
esp_err_t object_recovery_migrate_legacy_scan(
    const char *object_id
)
{
    if (object_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char legacy_path[OBJECT_RECOVERY_SCAN_PATH_SIZE];
    int written = snprintf(
        legacy_path,
        sizeof(legacy_path),
        "%s/%s/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        OBJECT_RECOVERY_SCAN_FILE_NAME
    );
    if (written <= 0 || written >= (int)sizeof(legacy_path)) {
        return ESP_OK;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return ESP_OK;
    }

    if (!object_recovery_file_exists(legacy_path)) {
        storage_manager_operation_end();
        return ESP_OK;
    }

    seismic_recorder_session_summary_t sessions[SEISMIC_RECORDER_MAX_SESSIONS];
    size_t session_count = 0U;
    const esp_err_t list_err = seismic_recorder_list_sessions(
        object_id, sessions, SEISMIC_RECORDER_MAX_SESSIONS, &session_count);
    if (list_err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "LEGACY_SCAN_MIGRATION object=%s state=SESSION_LIST_FAILED err=%s",
            object_id,
            esp_err_to_name(list_err)
        );
        storage_manager_operation_end();
        return ESP_OK;
    }

    const char *newest_session_id = object_recovery_newest_session_id(sessions, session_count);
    if (newest_session_id == NULL) {
        ESP_LOGW(
            TAG,
            "LEGACY_SCAN_MIGRATION object=%s state=NO_SESSION path=%s left_in_place=1",
            object_id,
            legacy_path
        );
        storage_manager_operation_end();
        return ESP_OK;
    }

    char destination_path[OBJECT_RECOVERY_SCAN_PATH_SIZE];
    written = snprintf(
        destination_path,
        sizeof(destination_path),
        "%s/%s/sessions/%s/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        newest_session_id,
        OBJECT_RECOVERY_SCAN_FILE_NAME
    );
    if (written <= 0 || written >= (int)sizeof(destination_path)) {
        ESP_LOGW(TAG, "LEGACY_SCAN_MIGRATION object=%s state=PATH_TOO_LONG", object_id);
        storage_manager_operation_end();
        return ESP_OK;
    }

    if (object_recovery_file_exists(destination_path)) {
        ESP_LOGW(
            TAG,
            "LEGACY_SCAN_MIGRATION object=%s session=%s state=DESTINATION_EXISTS left_in_place=1",
            object_id,
            newest_session_id
        );
        storage_manager_operation_end();
        return ESP_OK;
    }

    if (rename(legacy_path, destination_path) != 0) {
        ESP_LOGW(
            TAG,
            "LEGACY_SCAN_MIGRATION object=%s session=%s state=RENAME_FAILED",
            object_id,
            newest_session_id
        );
        storage_manager_operation_end();
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "LEGACY_SCAN_MIGRATION object=%s session=%s state=MIGRATED",
        object_id,
        newest_session_id
    );
    storage_manager_operation_end();
    return ESP_OK;
}
