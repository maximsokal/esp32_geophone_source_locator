#include "localization_run_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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


static void set_partial_error(
    localization_run_store_list_status_t *status,
    localization_run_store_partial_error_code_t code
)
{
    status->partial_error = true;
    ++status->partial_error_count;
    status->partial_error_codes |= (uint32_t)code;
}


/*
 * Enumerates valid run records while retaining the successfully parsed
 * entries when a matching file cannot be read or parsed. The status reports
 * how many failures occurred and which stable error-code categories applied.
 */
static esp_err_t localization_run_store_list_internal(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out,
    bool *directory_opened_out
)
{
    if (directory_opened_out != NULL) {
        *directory_opened_out = false;
    }
    if (
        records == NULL || max_records == 0U || record_count_out == NULL ||
        status_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status_out, 0, sizeof(*status_out));
    *record_count_out = 0U;

    char directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = build_session_directory_path(
        object_id, session_id, directory_path, sizeof(directory_path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    struct stat directory_info;
    errno = 0;
    if (stat(directory_path, &directory_info) != 0) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
        );
        return ESP_FAIL;
    }
    errno = 0;
    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
        );
        return ESP_FAIL;
    }
    if (directory_opened_out != NULL) {
        *directory_opened_out = true;
    }

    char *json = malloc(LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    if (json == NULL) {
        closedir(directory);
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
        );
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0U;
    struct dirent *entry;
    size_t max_count = max_records;
    if (max_count > LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION) {
        max_count = LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION;
    }
    esp_err_t result = ESP_OK;
    while (count < max_count) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                set_partial_error(
                    status_out,
                    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
                );
                result = ESP_FAIL;
            }
            break;
        }
        if (!is_run_record_filename(entry->d_name)) {
            continue;
        }

        char file_path[LOCALIZATION_RUN_STORE_PATH_MAX];
        const int written = snprintf(
            file_path, sizeof(file_path), "%s/%s", directory_path, entry->d_name
        );
        if (written < 0 || written >= (int)sizeof(file_path)) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
            );
            continue;
        }

        if (storage_manager_read_text(file_path, json, LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE) != ESP_OK) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED
            );
            continue;
        }

        if (localization_run_record_from_json(json, &records[count]) != ESP_OK) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED
            );
            continue;
        }

        ++count;
    }

    free(json);
    if (closedir(directory) != 0) {
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED
        );
        result = ESP_FAIL;
    }

    *record_count_out = count;
    return result;
}


esp_err_t localization_run_store_list_ex(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
)
{
    return localization_run_store_list_internal(
        object_id, session_id, records, max_records, record_count_out, status_out, NULL
    );
}


esp_err_t localization_run_store_list(
    const char *object_id,
    const char *session_id,
    localization_run_record_t *records,
    size_t max_records,
    size_t *record_count_out
)
{
    localization_run_store_list_status_t status;
    bool directory_opened = false;
    const esp_err_t result = localization_run_store_list_internal(
        object_id, session_id, records, max_records, record_count_out, &status,
        &directory_opened
    );
    if (result == ESP_FAIL) {
        return directory_opened ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    return result;
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


static esp_err_t localization_run_store_list_picks_internal(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out,
    bool *directory_opened_out
)
{
    if (directory_opened_out != NULL) {
        *directory_opened_out = false;
    }
    if (
        records == NULL || max_records == 0U || record_count_out == NULL ||
        status_out == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status_out, 0, sizeof(*status_out));
    *record_count_out = 0U;

    char directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const esp_err_t path_err = build_session_directory_path(
        object_id, session_id, directory_path, sizeof(directory_path)
    );
    if (path_err != ESP_OK) {
        return path_err;
    }

    struct stat directory_info;
    errno = 0;
    if (stat(directory_path, &directory_info) != 0) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
        );
        return ESP_FAIL;
    }
    errno = 0;
    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return ESP_ERR_NOT_FOUND;
        }
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
        );
        return ESP_FAIL;
    }
    if (directory_opened_out != NULL) {
        *directory_opened_out = true;
    }

    char *json = malloc(LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    if (json == NULL) {
        closedir(directory);
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
        );
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0U;
    struct dirent *entry;
    size_t max_count = max_records;
    if (max_count > LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION) {
        max_count = LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION;
    }
    esp_err_t result = ESP_OK;
    while (count < max_count) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                set_partial_error(
                    status_out,
                    LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
                );
                result = ESP_FAIL;
            }
            break;
        }
        if (!is_pick_record_filename(entry->d_name)) {
            continue;
        }

        char file_path[LOCALIZATION_RUN_STORE_PATH_MAX];
        const int written = snprintf(
            file_path, sizeof(file_path), "%s/%s", directory_path, entry->d_name
        );
        if (written < 0 || written >= (int)sizeof(file_path)) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
            );
            continue;
        }

        if (storage_manager_read_text(file_path, json, LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE) != ESP_OK) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
            );
            continue;
        }

        if (localization_pick_record_from_json(json, &records[count]) != ESP_OK) {
            set_partial_error(
                status_out,
                LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED
            );
            continue;
        }

        ++count;
    }

    free(json);
    if (closedir(directory) != 0) {
        set_partial_error(
            status_out,
            LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_LISTING_FAILED
        );
        result = ESP_FAIL;
    }

    *record_count_out = count;
    return result;
}


esp_err_t localization_run_store_list_picks_ex(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out,
    localization_run_store_list_status_t *status_out
)
{
    return localization_run_store_list_picks_internal(
        object_id, session_id, records, max_records, record_count_out, status_out, NULL
    );
}


esp_err_t localization_run_store_list_picks(
    const char *object_id,
    const char *session_id,
    localization_pick_record_t *records,
    size_t max_records,
    size_t *record_count_out
)
{
    localization_run_store_list_status_t status;
    bool directory_opened = false;
    const esp_err_t result = localization_run_store_list_picks_internal(
        object_id, session_id, records, max_records, record_count_out, &status,
        &directory_opened
    );
    if (result == ESP_FAIL) {
        return directory_opened ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    return result;
}
