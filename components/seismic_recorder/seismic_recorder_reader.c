#include "seismic_recorder_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "storage_manager.h"


typedef struct
{
    const char *path;

} seismic_recorder_bounded_reader_t;


static esp_err_t seismic_recorder_reader_errno_to_error(
    int error_number
)
{
    return error_number == ENOENT
        ? ESP_ERR_NOT_FOUND
        : ESP_FAIL;
}


static esp_err_t seismic_recorder_reader_file_size(
    const char *path,
    uint64_t *file_size
)
{
    if (path == NULL || file_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *file_size = 0U;

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        err = seismic_recorder_reader_errno_to_error(errno);
    }

    if (err == ESP_OK && fseek(file, 0L, SEEK_END) != 0) {
        err = ESP_FAIL;
    }

    long end = 0L;
    if (err == ESP_OK) {
        end = ftell(file);
        if (end < 0) {
            err = ESP_FAIL;
        }
    }

    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();

    if (err == ESP_OK) {
        *file_size = (uint64_t)end;
    }
    return err;
}


static esp_err_t seismic_recorder_reader_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    seismic_recorder_bounded_reader_t *reader =
        (seismic_recorder_bounded_reader_t *)context;

    if (
        reader == NULL ||
        reader->path == NULL ||
        buffer == NULL ||
        size_bytes == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset_bytes > (uint64_t)LONG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(reader->path, "rb");
    if (file == NULL) {
        err = seismic_recorder_reader_errno_to_error(errno);
    }

    if (err == ESP_OK && fseek(file, (long)offset_bytes, SEEK_SET) != 0) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK && fread(buffer, 1U, size_bytes, file) != size_bytes) {
        err = feof(file)
            ? ESP_ERR_INVALID_RESPONSE
            : ESP_FAIL;
    }

    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
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

    uint64_t file_size = 0U;
    err = seismic_recorder_reader_file_size(
        session_path,
        &file_size
    );
    if (err != ESP_OK) {
        free(session_path);
        return err;
    }

    seismic_frame_t *staging = heap_caps_malloc(
        sizeof(*staging) * SEISMIC_RECORDER_READ_STAGING_FRAMES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (staging == NULL) {
        free(session_path);
        return ESP_ERR_NO_MEM;
    }

    /*
     * CONFIG_FATFS_FS_LOCK=0 means an unlocked long-lived FILE * cannot be
     * made safe against concurrent object/session mutation by libc itself.
     * Keep no handle across arbitration windows: every read_at callback opens,
     * seeks, reads one bounded chunk, closes, and only then releases SPI2.
     * The CPU min/max reduction runs outside storage_manager entirely.
     */
    seismic_recorder_bounded_reader_t reader = {
        .path = session_path,
    };
    err = seismic_recorder_waveform_read_source(
        seismic_recorder_reader_read_at,
        &reader,
        file_size,
        &summary,
        start_frame,
        frame_count,
        max_points,
        staging,
        SEISMIC_RECORDER_READ_STAGING_FRAMES,
        points,
        point_capacity,
        info
    );

    heap_caps_free(staging);
    free(session_path);

    if (err == ESP_OK) {
        snprintf(info->object_id, sizeof(info->object_id), "%s", object_id);
    }
    return err;
}
