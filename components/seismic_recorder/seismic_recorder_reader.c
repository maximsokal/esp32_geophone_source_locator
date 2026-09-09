#include "seismic_recorder_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage_manager.h"


static esp_err_t seismic_recorder_reader_errno_to_error(
    int error_number
)
{
    return error_number == ENOENT
        ? ESP_ERR_NOT_FOUND
        : ESP_FAIL;
}


esp_err_t seismic_recorder_read_session(
    const char *object_id,
    const char *session_id,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t max_points,
    seismic_recorder_waveform_point_t *points,
    size_t point_capacity,
    seismic_recorder_waveform_info_t *info
)
{
    if (
        object_id == NULL ||
        session_id == NULL ||
        points == NULL ||
        info == NULL ||
        max_points == 0U ||
        max_points > SEISMIC_RECORDER_MAX_WAVEFORM_POINTS ||
        point_capacity < max_points
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    object_store_profile_t *profile = malloc(sizeof(*profile));
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = object_store_get_profile(object_id, profile);
    free(profile);
    if (err != ESP_OK) {
        return err;
    }

    seismic_recorder_session_summary_t summary;
    err = seismic_recorder_json_read_summary(
        object_id,
        session_id,
        &summary
    );
    if (err != ESP_OK) {
        return err;
    }

    if (!summary.complete) {
        return ESP_ERR_INVALID_STATE;
    }

    char *session_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (session_path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = seismic_recorder_fs_build_session_path(
        object_id,
        session_id,
        session_path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    if (err == ESP_OK) {
        const size_t path_length = strlen(session_path);
        static const char samples_suffix[] = "/samples.bin";
        const size_t suffix_length = sizeof(samples_suffix) - 1U;
        if (path_length + suffix_length >= SEISMIC_RECORDER_PATH_SIZE) {
            err = ESP_ERR_INVALID_SIZE;
        }
        else {
            memcpy(
                session_path + path_length,
                samples_suffix,
                suffix_length + 1U
            );
        }
    }

    if (err != ESP_OK) {
        free(session_path);
        return err;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(session_path);
        return err;
    }

    FILE *samples_file = NULL;
    samples_file = fopen(session_path, "rb");
    if (samples_file == NULL) {
        err = seismic_recorder_reader_errno_to_error(errno);
    }

    uint64_t file_size = 0U;
    if (err == ESP_OK) {
        if (fseek(samples_file, 0L, SEEK_END) != 0) {
            err = ESP_FAIL;
        }
        else {
            const long end = ftell(samples_file);
            if (end < 0) {
                err = ESP_FAIL;
            }
            else {
                file_size = (uint64_t)end;
            }
        }
    }

    if (err == ESP_OK) {
        err = seismic_recorder_waveform_read_file(
            samples_file,
            file_size,
            &summary,
            start_frame,
            frame_count,
            max_points,
            points,
            point_capacity,
            info
        );
        if (err == ESP_OK) {
            snprintf(info->object_id, sizeof(info->object_id), "%s", object_id);
        }
    }

    if (samples_file != NULL && fclose(samples_file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    free(session_path);
    return err;
}
