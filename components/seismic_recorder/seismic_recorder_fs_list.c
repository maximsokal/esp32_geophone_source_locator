#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object_format.h"
#include "seismic_recorder_session_selection.h"
#include "storage_manager.h"


typedef enum
{
    SEISMIC_RECORDER_LIST_METADATA_NONE = 0,
    SEISMIC_RECORDER_LIST_METADATA_PRIMARY,
    SEISMIC_RECORDER_LIST_METADATA_BACKUP,

} seismic_recorder_list_metadata_state_t;


static esp_err_t seismic_recorder_list_errno_to_error(int error_number)
{
    if (error_number == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
}


static esp_err_t seismic_recorder_list_sessions_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    if (
        !object_format_id_is_valid(object_id) ||
        path == NULL ||
        path_size == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        path,
        path_size,
        "%s/%s/sessions",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id
    );
    return written < 0 || written >= (int)path_size
        ? ESP_ERR_INVALID_SIZE
        : ESP_OK;
}


static esp_err_t seismic_recorder_list_metadata_state(
    const char *sessions_path,
    const char *session_id,
    char *session_path,
    size_t session_path_size,
    seismic_recorder_list_metadata_state_t *state
)
{
    if (
        sessions_path == NULL ||
        session_id == NULL ||
        session_path == NULL ||
        session_path_size == 0U ||
        state == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *state = SEISMIC_RECORDER_LIST_METADATA_NONE;
    const int written = snprintf(
        session_path,
        session_path_size,
        "%s/%s",
        sessions_path,
        session_id
    );
    if (written < 0 || written >= (int)session_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    DIR *directory = opendir(session_path);
    if (directory == NULL) {
        const int open_errno = errno;
        if (open_errno == ENOENT || open_errno == ENOTDIR) {
            return ESP_OK;
        }
        return seismic_recorder_list_errno_to_error(open_errno);
    }

    errno = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, "session.json") == 0) {
            *state = SEISMIC_RECORDER_LIST_METADATA_PRIMARY;
            break;
        }
        if (strcmp(entry->d_name, "session.json.bak") == 0) {
            *state = SEISMIC_RECORDER_LIST_METADATA_BACKUP;
        }
    }

    const int read_errno = errno;
    const int close_result = closedir(directory);
    if (read_errno != 0) {
        return seismic_recorder_list_errno_to_error(read_errno);
    }
    if (close_result != 0) {
        return seismic_recorder_list_errno_to_error(errno);
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_fs_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
)
{
    if (
        object_id == NULL ||
        sessions == NULL ||
        session_count == NULL ||
        max_sessions == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *session_count = 0U;

    object_store_profile_t *profile = malloc(sizeof(*profile));
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = object_store_get_profile(object_id, profile);
    free(profile);
    if (err != ESP_OK) {
        return err;
    }

    char *sessions_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    char *session_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (sessions_path == NULL || session_path == NULL) {
        free(session_path);
        free(sessions_path);
        return ESP_ERR_NO_MEM;
    }

    seismic_recorder_session_selection_t selection;
    seismic_recorder_session_selection_init(&selection, max_sessions);

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(session_path);
        free(sessions_path);
        return err;
    }

    err = seismic_recorder_list_sessions_path(
        object_id,
        sessions_path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    DIR *directory = NULL;
    if (err == ESP_OK) {
        directory = opendir(sessions_path);
        if (directory == NULL) {
            err = seismic_recorder_list_errno_to_error(errno);
        }
    }

    if (err == ESP_OK) {
        while (true) {
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (entry == NULL) {
                if (errno != 0) {
                    err = seismic_recorder_list_errno_to_error(errno);
                }
                break;
            }

            seismic_recorder_session_selection_consider(
                &selection,
                entry->d_name
            );
        }
    }

    if (directory != NULL) {
        const int close_result = closedir(directory);
        if (err == ESP_OK && close_result != 0) {
            err = seismic_recorder_list_errno_to_error(errno);
        }
    }

    /* Do not publish metadata from a partial parent census. Once the complete
     * membership is known, read only the retained newest N inside the same
     * ordinary storage operation. */
    if (err == ESP_OK) {
        const size_t selected_count =
            seismic_recorder_session_selection_count(&selection);
        for (size_t i = 0U; i < selected_count; ++i) {
            const char *session_id =
                seismic_recorder_session_selection_id(&selection, i);
            seismic_recorder_session_summary_t summary;
            memset(&summary, 0, sizeof(summary));

            seismic_recorder_list_metadata_state_t metadata_state =
                SEISMIC_RECORDER_LIST_METADATA_NONE;
            const esp_err_t metadata_err =
                seismic_recorder_list_metadata_state(
                    sessions_path,
                    session_id,
                    session_path,
                    SEISMIC_RECORDER_PATH_SIZE,
                    &metadata_state
                );
            if (metadata_err != ESP_OK) {
                err = metadata_err;
                break;
            }

            if (
                metadata_state == SEISMIC_RECORDER_LIST_METADATA_NONE ||
                (
                    metadata_state == SEISMIC_RECORDER_LIST_METADATA_PRIMARY
                        ? seismic_recorder_json_read_summary_known_present(
                            object_id,
                            session_id,
                            &summary
                        )
                        : seismic_recorder_json_read_summary(
                            object_id,
                            session_id,
                            &summary
                        )
                ) != ESP_OK
            ) {
                static const size_t canonical_size = sizeof("S000000");
                memcpy(summary.session_id, session_id, canonical_size);
                summary.complete = false;
            }

            sessions[*session_count] = summary;
            (*session_count)++;
        }
    }

    storage_manager_operation_end();
    free(session_path);
    free(sessions_path);
    return err;
}
