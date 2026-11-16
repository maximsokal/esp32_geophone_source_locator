#include "object_store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "object_format.h"
#include "storage_manager.h"
#include "scan_artifact_path.h"

#ifndef OBJECT_STORE_STAT_FN
#define OBJECT_STORE_STAT_FN stat
#else
int OBJECT_STORE_STAT_FN(const char *path, struct stat *info);
#endif

#define OBJECT_STORE_SCAN_NAME "scan-v1.bin"
#define OBJECT_STORE_SESSION_ID_LENGTH 7U
#define OBJECT_STORE_SCAN_PATH_SIZE \
    (sizeof(STORAGE_MANAGER_OBJECTS_PATH) + OBJECT_STORE_ID_SIZE + \
     OBJECT_STORE_SESSION_ID_LENGTH + sizeof("/sessions//" OBJECT_STORE_SCAN_NAME))

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

static bool object_store_session_id_valid(const char *session_id)
{
    if (session_id == NULL || strlen(session_id) != OBJECT_STORE_SESSION_ID_LENGTH ||
        session_id[0] != 'S') return false;
    for (size_t i = 1U; i < OBJECT_STORE_SESSION_ID_LENGTH; ++i) {
        if (session_id[i] < '0' || session_id[i] > '9') return false;
    }
    return true;
}

static esp_err_t object_store_artifact_path(
    const char *object_id,
    const char *name,
    char path[OBJECT_STORE_FS_PATH_SIZE]
)
{
    const esp_err_t err = object_store_fs_object_path(object_id, path, OBJECT_STORE_FS_PATH_SIZE);
    if (err != ESP_OK) return err;
    const size_t length = strlen(path);
    const int written = snprintf(path + length, OBJECT_STORE_FS_PATH_SIZE - length, "/%s", name);
    return written < 0 || (size_t)written >= OBJECT_STORE_FS_PATH_SIZE - length
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}

esp_err_t object_store_set_calc_settings_json(const char *object_id, const char *json)
{
    if (!object_format_id_is_valid(object_id) || json == NULL) return ESP_ERR_INVALID_ARG;
    size_t length = 0U;
    while (length < OBJECT_STORE_CALC_SETTINGS_JSON_SIZE && json[length] != '\0') ++length;
    if (length == 0U || length >= OBJECT_STORE_CALC_SETTINGS_JSON_SIZE) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;
    if (!object_store_fs_object_exists(object_id)) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        char path[OBJECT_STORE_FS_PATH_SIZE];
        err = object_store_artifact_path(object_id, "calc_settings.json", path);
        if (err == ESP_OK) err = storage_manager_atomic_write_text(path, json);
        if (err == ESP_OK) {
            uint64_t generation = 0U;
            if (object_store_cache_generation_bump(object_id, &generation)) {
                object_store_profile_cache_note_calc_settings_saved_locked(
                    object_id,
                    generation
                );
            }
        }
    }
    object_store_internal_end_file_operation();
    return err;
}

static bool object_store_calc_cache_copy_locked(
    const char *object_id,
    uint64_t generation,
    char **json_out,
    esp_err_t *result_out
)
{
    const object_store_cache_calc_state_t state =
        object_store_cache_calc_state(object_id, generation);
    if (state == OBJECT_STORE_CACHE_CALC_MISS) return false;
    if (state == OBJECT_STORE_CACHE_CALC_ABSENT) {
        *result_out = ESP_ERR_NOT_FOUND;
        return true;
    }

    char *json = malloc(OBJECT_STORE_CALC_SETTINGS_JSON_SIZE);
    if (json == NULL) {
        *result_out = ESP_ERR_NO_MEM;
        return true;
    }
    if (!object_store_cache_calc_copy(
            object_id,
            generation,
            json,
            OBJECT_STORE_CALC_SETTINGS_JSON_SIZE)) {
        free(json);
        return false;
    }
    *json_out = json;
    *result_out = ESP_OK;
    return true;
}

static esp_err_t object_store_calc_cache_require_storage(void)
{
    if (storage_manager_is_ready()) return ESP_OK;

    if (object_store_internal_begin_state_operation() == ESP_OK) {
        object_store_cache_reset();
        object_store_internal_end_state_operation();
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t object_store_get_calc_settings_json_cached(const char *object_id, char **json_out)
{
    if (!object_format_id_is_valid(object_id) || json_out == NULL) return ESP_ERR_INVALID_ARG;
    *json_out = NULL;

    esp_err_t err = object_store_calc_cache_require_storage();
    if (err != ESP_OK) return err;

    err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) return err;
    uint64_t generation = 0U;
    bool generation_available =
        object_store_cache_generation_get(object_id, &generation);
    esp_err_t cached_result = ESP_OK;
    if (generation_available && object_store_calc_cache_copy_locked(
            object_id,
            generation,
            json_out,
            &cached_result)) {
        object_store_internal_end_state_operation();
        return cached_result;
    }
    object_store_internal_end_state_operation();

    err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    generation_available = object_store_cache_generation_get(object_id, &generation);
    cached_result = ESP_OK;
    if (generation_available && object_store_calc_cache_copy_locked(
            object_id,
            generation,
            json_out,
            &cached_result)) {
        object_store_internal_end_file_operation();
        return cached_result;
    }

    object_store_internal_end_state_operation();
    const bool object_present = object_store_fs_object_exists(object_id);
    if (!object_present) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        char path[OBJECT_STORE_FS_PATH_SIZE];
        char *json = NULL;
        err = object_store_artifact_path(object_id, "calc_settings.json", path);
        if (err == ESP_OK) {
            json = malloc(OBJECT_STORE_CALC_SETTINGS_JSON_SIZE);
            if (json == NULL) err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK) {
            err = storage_manager_read_text(path, json, OBJECT_STORE_CALC_SETTINGS_JSON_SIZE);
            if (err == ESP_OK) *json_out = json;
            else free(json);
        }
    }

    if (object_store_internal_begin_state_operation() == ESP_OK) {
        if (generation_available && object_present &&
            (err == ESP_OK || err == ESP_ERR_NOT_FOUND) &&
            object_store_cache_generation_matches(object_id, generation)) {
            object_store_cache_calc_store(
                object_id,
                generation,
                err == ESP_OK ? *json_out : NULL,
                err == ESP_OK
            );
        }
        object_store_internal_end_file_operation();
    } else {
        storage_manager_operation_end();
        if (err == ESP_OK) {
            free(*json_out);
            *json_out = NULL;
            err = ESP_ERR_INVALID_STATE;
        }
    }
    return err;
}

esp_err_t object_store_get_calc_settings_json(const char *object_id, char **json_out)
{
    return object_store_get_calc_settings_json_cached(object_id, json_out);
}

esp_err_t object_store_scan_present(
    const char *object_id,
    const char *session_id,
    bool *present_out
)
{
    if (!object_format_id_is_valid(object_id) || session_id == NULL || present_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *present_out = false;
    const bool staged = session_id[0] == '\0';
    if (!staged && !object_store_session_id_valid(session_id)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;
    if (!object_store_fs_object_exists(object_id)) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        char path[OBJECT_STORE_SCAN_PATH_SIZE];
        /* A session either holds its own scan (older recordings) or refers to the shared one of its sensor placement. */
        int written = staged
            ? snprintf(path, sizeof(path), "%s/%s/staging/%s", STORAGE_MANAGER_OBJECTS_PATH,
                object_id, OBJECT_STORE_SCAN_NAME)
            : scan_artifact_resolve(STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, OBJECT_STORE_SCAN_NAME,
                path, sizeof(path));
        if (written < 0 || written >= (int)sizeof(path)) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            struct stat info;
            if (OBJECT_STORE_STAT_FN(path, &info) == 0) {
                *present_out = S_ISREG(info.st_mode);
            } else if (errno != ENOENT && errno != ENOTDIR) {
                err = ESP_FAIL;
            }
        }
    }
    object_store_internal_end_file_operation();
    return err;
}
