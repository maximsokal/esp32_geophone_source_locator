#include "localization_run_store.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object_format.h"
#include "storage_manager.h"


static bool is_canonical_session_id(
    const char *session_id
)
{
    if (session_id == NULL) {
        return false;
    }

    static const size_t canonical_length = sizeof("S000000") - 1u;
    if (strlen(session_id) != canonical_length || session_id[0] != 'S') {
        return false;
    }
    for (size_t i = 1u; i < canonical_length; ++i) {
        if (!isdigit((unsigned char)session_id[i])) {
            return false;
        }
    }
    return true;
}


static bool is_filename_safe(
    const char *text
)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '_') {
            return false;
        }
    }
    return true;
}


static const char *kind_slug(
    localization_run_kind_t kind
)
{
    switch (kind) {
        case LOCALIZATION_RUN_KIND_JOINT_FUSION:
            return "joint";
        case LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD:
        default:
            return "individual";
    }
}


esp_err_t localization_run_store_build_path(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record,
    char *path_out,
    size_t path_out_size
)
{
    if (record == NULL || path_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!object_format_id_is_valid(object_id) || !is_canonical_session_id(session_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_filename_safe(record->method_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        path_out,
        path_out_size,
        "%s/%s/sessions/%s/localization_run_%s_%s_%llu.json",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id,
        kind_slug(record->kind),
        record->method_name,
        (unsigned long long)record->computed_at_ms
    );

    return
        written < 0 || written >= (int)path_out_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


esp_err_t localization_run_store_write(
    const char *object_id,
    const char *session_id,
    const localization_run_record_t *record
)
{
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = localization_run_store_build_path(
        object_id, session_id, record, path, sizeof(path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    char *json = NULL;
    const esp_err_t json_err = localization_run_record_to_json(record, &json);
    if (json_err != ESP_OK) {
        return json_err;
    }

    const esp_err_t write_err = storage_manager_atomic_write_text(path, json);
    free(json);
    return write_err;
}


static esp_err_t build_session_directory_path(
    const char *object_id,
    const char *session_id,
    char *path_out,
    size_t path_out_size
)
{
    if (!object_format_id_is_valid(object_id) || !is_canonical_session_id(session_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        path_out,
        path_out_size,
        "%s/%s/sessions/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id
    );

    return
        written < 0 || written >= (int)path_out_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


static bool is_run_record_filename(
    const char *name
)
{
    static const char prefix[] = "localization_run_";
    static const char suffix[] = ".json";
    const size_t name_length = strlen(name);
    const size_t prefix_length = sizeof(prefix) - 1U;
    const size_t suffix_length = sizeof(suffix) - 1U;

    return
        name_length > prefix_length + suffix_length &&
        strncmp(name, prefix, prefix_length) == 0 &&
        strcmp(name + name_length - suffix_length, suffix) == 0;
}


/*
 * MAX-168: enumerate every localization run already persisted for one
 * session -- the WebUI needs this to show past "Calculate source" results,
 * and a caller deciding whether to recompute needs it to see what already
 * exists. Every run lives in its own file (localization_run_store_write()'s
 * own doc comment explains why), so this is a directory listing, not a
 * single-record read.
 *
 * A file that fails to read or parse is skipped rather than failing the
 * whole listing -- one corrupt run record must not hide every other valid
 * one for the same session. `record_count_out` reports how many were
 * actually parsed into `records`; it does not distinguish "no runs yet"
 * from "some runs were unreadable", since neither is actionable
 * differently by a caller that just wants what is available now.
 */
esp_err_t localization_run_store_list(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out
)
{
    if (records == NULL || max_records == 0U || record_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *record_count_out = 0U;

    char directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = build_session_directory_path(
        object_id, session_id, directory_path, sizeof(directory_path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *json = malloc(LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    if (json == NULL) {
        closedir(directory);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0U;
    struct dirent *entry;
    while (count < max_records && (entry = readdir(directory)) != NULL) {
        if (!is_run_record_filename(entry->d_name)) {
            continue;
        }

        char file_path[LOCALIZATION_RUN_STORE_PATH_MAX];
        const int written = snprintf(
            file_path, sizeof(file_path), "%s/%s", directory_path, entry->d_name
        );
        if (written < 0 || written >= (int)sizeof(file_path)) {
            continue;
        }

        if (storage_manager_read_text(file_path, json, LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE) != ESP_OK) {
            continue;
        }

        if (localization_run_record_from_json(json, &records[count]) != ESP_OK) {
            continue;
        }

        ++count;
    }

    free(json);
    closedir(directory);

    *record_count_out = count;
    return ESP_OK;
}


esp_err_t localization_run_store_build_picks_path(
    const char *object_id,
    const char *session_id,
    uint64_t computed_at_ms,
    char *path_out,
    size_t path_out_size
)
{
    if (path_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!object_format_id_is_valid(object_id) || !is_canonical_session_id(session_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        path_out,
        path_out_size,
        "%s/%s/sessions/%s/localization_picks_%llu.json",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id,
        (unsigned long long)computed_at_ms
    );

    return
        written < 0 || written >= (int)path_out_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


esp_err_t localization_run_store_write_picks(
    const char *object_id,
    const char *session_id,
    const localization_pick_record_t *record
)
{
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = localization_run_store_build_picks_path(
        object_id, session_id, record->computed_at_ms, path, sizeof(path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    char *json = NULL;
    const esp_err_t json_err = localization_pick_record_to_json(record, &json);
    if (json_err != ESP_OK) {
        return json_err;
    }

    const esp_err_t write_err = storage_manager_atomic_write_text(path, json);
    free(json);
    return write_err;
}


static bool is_pick_record_filename(
    const char *name
)
{
    static const char prefix[] = "localization_picks_";
    static const char suffix[] = ".json";
    const size_t name_length = strlen(name);
    const size_t prefix_length = sizeof(prefix) - 1U;
    const size_t suffix_length = sizeof(suffix) - 1U;

    return
        name_length > prefix_length + suffix_length &&
        strncmp(name, prefix, prefix_length) == 0 &&
        strcmp(name + name_length - suffix_length, suffix) == 0;
}


esp_err_t localization_run_store_list_picks(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out
)
{
    if (records == NULL || max_records == 0U || record_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *record_count_out = 0U;

    char directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = build_session_directory_path(
        object_id, session_id, directory_path, sizeof(directory_path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char *json = malloc(LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    if (json == NULL) {
        closedir(directory);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0U;
    struct dirent *entry;
    while (count < max_records && (entry = readdir(directory)) != NULL) {
        if (!is_pick_record_filename(entry->d_name)) {
            continue;
        }

        char file_path[LOCALIZATION_RUN_STORE_PATH_MAX];
        const int written = snprintf(
            file_path, sizeof(file_path), "%s/%s", directory_path, entry->d_name
        );
        if (written < 0 || written >= (int)sizeof(file_path)) {
            continue;
        }

        if (storage_manager_read_text(file_path, json, LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE) != ESP_OK) {
            continue;
        }

        if (localization_pick_record_from_json(json, &records[count]) != ESP_OK) {
            continue;
        }

        ++count;
    }

    free(json);
    closedir(directory);

    *record_count_out = count;
    return ESP_OK;
}
