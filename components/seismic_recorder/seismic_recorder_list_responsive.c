#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "object_format.h"
#include "storage_manager.h"


typedef enum
{
    SEISMIC_RECORDER_RESP_METADATA_NONE = 0,
    SEISMIC_RECORDER_RESP_METADATA_PRIMARY,
    SEISMIC_RECORDER_RESP_METADATA_BACKUP,

} seismic_recorder_responsive_metadata_state_t;


static bool seismic_recorder_responsive_parse_session_id(
    const char *name
)
{
    if (name == NULL) {
        return false;
    }

    static const size_t canonical_length = sizeof("S000000") - 1U;
    if (strlen(name) != canonical_length || name[0] != 'S') {
        return false;
    }

    unsigned parsed = 0U;
    for (size_t index = 1U; index < canonical_length; ++index) {
        const char digit = name[index];
        if (digit < '0' || digit > '9') {
            return false;
        }
        parsed = parsed * 10U + (unsigned)(digit - '0');
    }

    return parsed > 0U && parsed <= 999999U;
}


static esp_err_t seismic_recorder_responsive_errno_to_error(
    int error_number
)
{
    if (error_number == ENOENT || error_number == ENOTDIR) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
}


static esp_err_t seismic_recorder_responsive_sessions_path(
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


static esp_err_t seismic_recorder_responsive_metadata_state(
    const char *sessions_path,
    const char *session_id,
    seismic_recorder_responsive_metadata_state_t *state
)
{
    if (sessions_path == NULL || session_id == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *state = SEISMIC_RECORDER_RESP_METADATA_NONE;

    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    const int written = snprintf(
        session_path,
        sizeof(session_path),
        "%s/%s",
        sessions_path,
        session_id
    );
    if (written < 0 || written >= (int)sizeof(session_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    DIR *directory = opendir(session_path);
    if (directory == NULL) {
        const int open_errno = errno;
        if (open_errno == ENOENT || open_errno == ENOTDIR) {
            /* The child can disappear after the parent census. Represent it
             * as an incomplete session snapshot rather than failing refresh. */
            return ESP_OK;
        }
        return seismic_recorder_responsive_errno_to_error(open_errno);
    }

    errno = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, "session.json") == 0) {
            *state = SEISMIC_RECORDER_RESP_METADATA_PRIMARY;
            break;
        }
        if (strcmp(entry->d_name, "session.json.bak") == 0) {
            *state = SEISMIC_RECORDER_RESP_METADATA_BACKUP;
        }
    }

    const int read_errno = errno;
    const int close_result = closedir(directory);
    if (read_errno != 0) {
        return seismic_recorder_responsive_errno_to_error(read_errno);
    }
    if (close_result != 0) {
        return seismic_recorder_responsive_errno_to_error(errno);
    }

    return ESP_OK;
}


static void seismic_recorder_responsive_sort_sessions(
    seismic_recorder_session_summary_t *sessions,
    size_t count
)
{
    for (size_t i = 0U; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            if (strcmp(sessions[i].session_id, sessions[j].session_id) >= 0) {
                continue;
            }

            seismic_recorder_session_summary_t temp = sessions[i];
            sessions[i] = sessions[j];
            sessions[j] = temp;
        }
    }
}


esp_err_t seismic_recorder_list_sessions_responsive(
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

    /* Preserve the public recorder API's initialized-state contract. */
    seismic_recorder_snapshot_t recorder_snapshot;
    esp_err_t err = seismic_recorder_get_snapshot(&recorder_snapshot);
    if (err != ESP_OK) {
        return err;
    }

    object_store_profile_t *profile = malloc(sizeof(*profile));
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = object_store_get_profile(object_id, profile);
    free(profile);
    if (err != ESP_OK) {
        return err;
    }

    char sessions_path[SEISMIC_RECORDER_PATH_SIZE];
    err = seismic_recorder_responsive_sessions_path(
        object_id,
        sessions_path,
        sizeof(sessions_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    const size_t bounded =
        max_sessions < SEISMIC_RECORDER_MAX_SESSIONS
            ? max_sessions
            : SEISMIC_RECORDER_MAX_SESSIONS;

    char session_ids[SEISMIC_RECORDER_MAX_SESSIONS]
                    [SEISMIC_RECORDER_SESSION_ID_SIZE];
    size_t id_count = 0U;

    /* Parent directory census is one bounded storage/TFT window. */
    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory = opendir(sessions_path);
    if (directory == NULL) {
        err = seismic_recorder_responsive_errno_to_error(errno);
    }
    else {
        errno = 0;
        struct dirent *entry = NULL;
        while (id_count < bounded && (entry = readdir(directory)) != NULL) {
            if (!seismic_recorder_responsive_parse_session_id(entry->d_name)) {
                continue;
            }

            snprintf(
                session_ids[id_count],
                sizeof(session_ids[id_count]),
                "%s",
                entry->d_name
            );
            id_count++;
        }

        const int read_errno = errno;
        const int close_result = closedir(directory);
        if (read_errno != 0) {
            err = seismic_recorder_responsive_errno_to_error(read_errno);
        }
        else if (close_result != 0) {
            err = seismic_recorder_responsive_errno_to_error(errno);
        }
    }

    storage_manager_operation_end();

    if (err != ESP_OK) {
        return err;
    }

    /* Each metadata read gets its own storage arbitration window. This keeps
     * the low-priority Web cache from holding the TFT quiesced for the whole
     * session history while preserving the same per-session recovery rules. */
    for (size_t i = 0U; i < id_count; ++i) {
        seismic_recorder_session_summary_t summary;
        memset(&summary, 0, sizeof(summary));

        err = storage_manager_operation_begin();
        if (err != ESP_OK) {
            return err;
        }

        seismic_recorder_responsive_metadata_state_t metadata_state =
            SEISMIC_RECORDER_RESP_METADATA_NONE;
        const esp_err_t metadata_err =
            seismic_recorder_responsive_metadata_state(
                sessions_path,
                session_ids[i],
                &metadata_state
            );

        if (metadata_err == ESP_OK) {
            const esp_err_t summary_err =
                metadata_state == SEISMIC_RECORDER_RESP_METADATA_PRIMARY
                    ? seismic_recorder_json_read_summary_known_present(
                        object_id,
                        session_ids[i],
                        &summary
                    )
                    : metadata_state == SEISMIC_RECORDER_RESP_METADATA_BACKUP
                        ? seismic_recorder_json_read_summary(
                            object_id,
                            session_ids[i],
                            &summary
                        )
                        : ESP_ERR_NOT_FOUND;

            if (summary_err != ESP_OK) {
                snprintf(
                    summary.session_id,
                    sizeof(summary.session_id),
                    "%s",
                    session_ids[i]
                );
                summary.complete = false;
            }
        }

        storage_manager_operation_end();

        if (metadata_err != ESP_OK) {
            return metadata_err;
        }

        sessions[*session_count] = summary;
        (*session_count)++;
    }

    seismic_recorder_responsive_sort_sessions(
        sessions,
        *session_count
    );

    return ESP_OK;
}
