#include "localization_run_store.h"

#include <ctype.h>
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
