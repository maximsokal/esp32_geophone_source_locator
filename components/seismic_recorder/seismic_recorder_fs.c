#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "storage_manager.h"


#define SEISMIC_RECORDER_BINARY_VERSION  1U


static esp_err_t seismic_recorder_fs_sessions_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    int written = snprintf(
        path,
        path_size,
        "%s/%s/sessions",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id
    );

    return
        written < 0 || written >= (int)path_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


static esp_err_t seismic_recorder_fs_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    int written = snprintf(
        path,
        path_size,
        "%s/%s/sessions/%s",
        STORAGE_MANAGER_OBJECTS_PATH,
        object_id,
        session_id
    );

    return
        written < 0 || written >= (int)path_size
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


static bool seismic_recorder_fs_parse_session_number(
    const char *name,
    unsigned *number
)
{
    if (name == NULL || number == NULL) {
        return false;
    }

    unsigned parsed = 0;
    char trailing = '\0';
    if (sscanf(name, "S%06u%c", &parsed, &trailing) != 1) {
        return false;
    }

    if (parsed == 0U || parsed > 999999U) {
        return false;
    }

    *number = parsed;
    return true;
}


static esp_err_t seismic_recorder_fs_next_session_id(
    const char *object_id,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE]
)
{
    char sessions_path[SEISMIC_RECORDER_PATH_SIZE];
    esp_err_t err = seismic_recorder_fs_sessions_path(
        object_id,
        sessions_path,
        sizeof(sessions_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    DIR *directory = opendir(sessions_path);
    if (directory == NULL) {
        return ESP_FAIL;
    }

    unsigned highest = 0U;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        unsigned number = 0U;
        if (
            seismic_recorder_fs_parse_session_number(
                entry->d_name,
                &number
            ) &&
            number > highest
        ) {
            highest = number;
        }
    }

    closedir(directory);

    if (highest >= 999999U) {
        return ESP_ERR_NO_MEM;
    }

    int written = snprintf(
        session_id,
        SEISMIC_RECORDER_SESSION_ID_SIZE,
        "S%06u",
        highest + 1U
    );

    return
        written < 0 || written >= SEISMIC_RECORDER_SESSION_ID_SIZE
            ? ESP_ERR_INVALID_SIZE
            : ESP_OK;
}


esp_err_t seismic_recorder_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        samples_file == NULL ||
        sample_rate_hz == 0U ||
        channel_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_file = NULL;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    err = seismic_recorder_fs_next_session_id(
        object_id,
        session_id
    );

    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    if (err == ESP_OK) {
        err = seismic_recorder_fs_session_path(
            object_id,
            session_id,
            session_path,
            sizeof(session_path)
        );
    }

    if (err == ESP_OK) {
        err = storage_manager_create_directory(session_path);
    }

    char samples_path[SEISMIC_RECORDER_PATH_SIZE];
    if (err == ESP_OK) {
        int written = snprintf(
            samples_path,
            sizeof(samples_path),
            "%s/samples.bin",
            session_path
        );
        if (written < 0 || written >= (int)sizeof(samples_path)) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }

    FILE *file = NULL;
    if (err == ESP_OK) {
        file = fopen(samples_path, "wb");
        if (file == NULL) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        const seismic_recorder_binary_header_t header = {
            .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
            .version = SEISMIC_RECORDER_BINARY_VERSION,
            .channel_count = channel_count,
            .sample_rate_hz = sample_rate_hz,
            .frame_size_bytes = sizeof(seismic_frame_t),
            .reserved = 0U,
        };

        if (fwrite(&header, sizeof(header), 1U, file) != 1U) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        if (file != NULL) {
            fclose(file);
        }
        if (session_id[0] != '\0') {
            char failed_path[SEISMIC_RECORDER_PATH_SIZE];
            if (
                seismic_recorder_fs_session_path(
                    object_id,
                    session_id,
                    failed_path,
                    sizeof(failed_path)
                ) == ESP_OK
            ) {
                (void)storage_manager_remove_tree(failed_path);
            }
        }
    }
    else {
        *samples_file = file;
    }

    storage_manager_operation_end();
    return err;
}


esp_err_t seismic_recorder_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
)
{
    if (
        samples_file == NULL ||
        frames == NULL ||
        frames_written == NULL ||
        frame_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *frames_written = 0U;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    const size_t written = fwrite(
        frames,
        sizeof(seismic_frame_t),
        frame_count,
        samples_file
    );

    *frames_written = written;
    if (written != frame_count || ferror(samples_file) != 0) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
}


esp_err_t seismic_recorder_fs_flush(
    FILE *samples_file,
    bool sync_to_media
)
{
    if (samples_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    if (fflush(samples_file) != 0) {
        err = ESP_FAIL;
    }
    else if (
        sync_to_media &&
        fsync(fileno(samples_file)) != 0
    ) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
}


esp_err_t seismic_recorder_fs_close(
    FILE *samples_file
)
{
    if (samples_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    if (fflush(samples_file) != 0) {
        err = ESP_FAIL;
    }
    else if (fsync(fileno(samples_file)) != 0) {
        err = ESP_FAIL;
    }

    if (fclose(samples_file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
}


static void seismic_recorder_fs_sort_sessions(
    seismic_recorder_session_summary_t *sessions,
    size_t count
)
{
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1U; j < count; ++j) {
            if (
                strcmp(
                    sessions[i].session_id,
                    sessions[j].session_id
                ) >= 0
            ) {
                continue;
            }

            seismic_recorder_session_summary_t temp = sessions[i];
            sessions[i] = sessions[j];
            sessions[j] = temp;
        }
    }
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

    object_store_profile_t profile;
    esp_err_t err = object_store_get_profile(object_id, &profile);
    if (err != ESP_OK) {
        return err;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    char sessions_path[SEISMIC_RECORDER_PATH_SIZE];
    err = seismic_recorder_fs_sessions_path(
        object_id,
        sessions_path,
        sizeof(sessions_path)
    );

    DIR *directory = NULL;
    if (err == ESP_OK) {
        directory = opendir(sessions_path);
        if (directory == NULL) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK) {
        struct dirent *entry = NULL;
        while (
            *session_count < max_sessions &&
            (entry = readdir(directory)) != NULL
        ) {
            unsigned number = 0U;
            if (!seismic_recorder_fs_parse_session_number(entry->d_name, &number)) {
                continue;
            }

            seismic_recorder_session_summary_t summary;
            memset(&summary, 0, sizeof(summary));

            if (
                seismic_recorder_json_read_summary(
                    object_id,
                    entry->d_name,
                    &summary
                ) != ESP_OK
            ) {
                const size_t session_id_length = strnlen(
                    entry->d_name,
                    sizeof(summary.session_id)
                );
                if (session_id_length >= sizeof(summary.session_id)) {
                    continue;
                }
                memcpy(
                    summary.session_id,
                    entry->d_name,
                    session_id_length + 1U
                );
                summary.complete = false;
            }

            sessions[*session_count] = summary;
            (*session_count)++;
        }
    }

    if (directory != NULL) {
        closedir(directory);
    }

    storage_manager_operation_end();

    if (err == ESP_OK) {
        seismic_recorder_fs_sort_sessions(
            sessions,
            *session_count
        );
    }

    return err;
}


esp_err_t seismic_recorder_fs_delete_session(
    const char *object_id,
    const char *session_id
)
{
    if (object_id == NULL || session_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned session_number = 0U;
    if (!seismic_recorder_fs_parse_session_number(session_id, &session_number)) {
        return ESP_ERR_INVALID_ARG;
    }

    object_store_profile_t profile;
    esp_err_t err = object_store_get_profile(object_id, &profile);
    if (err != ESP_OK) {
        return err;
    }

    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    err = seismic_recorder_fs_session_path(
        object_id,
        session_id,
        session_path,
        sizeof(session_path)
    );
    if (err != ESP_OK) {
        return err;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    err = storage_manager_remove_tree(session_path);

    storage_manager_operation_end();
    return err;
}
