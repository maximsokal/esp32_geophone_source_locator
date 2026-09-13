#include "seismic_recorder_lod_store.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#ifdef _MSC_VER
#define fileno _fileno
#endif
#else
#include <unistd.h>
#endif

#include "esp_log.h"
#include "seismic_recorder_internal.h"
#include "seismic_recorder_lod.h"
#include "storage_manager.h"


#define SEISMIC_RECORDER_LOD_APPEND_FRAMES 256U
#define SEISMIC_RECORDER_LOD_APPEND_RECORDS \
    ((SEISMIC_RECORDER_LOD_APPEND_FRAMES / SEISMIC_RECORDER_LOD_BLOCK_FRAMES) + 1U)


static const char *TAG = "SEISMIC_RECORDER_LOD";
static const char *LOD_FILE_NAME = "waveform-lod64.bin";

static FILE *s_lod_file = NULL;
static char s_lod_path[SEISMIC_RECORDER_PATH_SIZE];
static seismic_recorder_lod_builder_t s_lod_builder;


static void seismic_recorder_lod_store_clear_state(void)
{
    s_lod_file = NULL;
    s_lod_path[0] = '\0';
    seismic_recorder_lod_builder_reset(&s_lod_builder, 0U);
}


static void seismic_recorder_lod_store_invalidate_locked(void)
{
    FILE *file = s_lod_file;
    char path[SEISMIC_RECORDER_PATH_SIZE];
    snprintf(path, sizeof(path), "%s", s_lod_path);
    seismic_recorder_lod_store_clear_state();

    if (file != NULL && fclose(file) != 0) {
        ESP_LOGW(TAG, "Failed to close invalid LOD sidecar");
    }
    if (path[0] != '\0' && remove(path) != 0) {
        ESP_LOGW(TAG, "Failed to remove invalid LOD sidecar: %s", path);
    }
}


bool seismic_recorder_lod_store_is_active(void)
{
    return s_lod_file != NULL;
}


esp_err_t seismic_recorder_lod_store_begin(
    const char *session_path,
    uint32_t sample_rate_hz
)
{
    if (session_path == NULL || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lod_file != NULL) {
        seismic_recorder_lod_store_abort();
    }

    const int written = snprintf(
        s_lod_path,
        sizeof(s_lod_path),
        "%s/%s",
        session_path,
        LOD_FILE_NAME
    );
    if (written < 0 || written >= (int)sizeof(s_lod_path)) {
        seismic_recorder_lod_store_clear_state();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        seismic_recorder_lod_store_clear_state();
        return err;
    }

    FILE *file = fopen(s_lod_path, "wb");
    if (file == NULL) {
        err = ESP_FAIL;
    }

    if (err == ESP_OK) {
        seismic_recorder_lod_file_header_t header;
        seismic_recorder_lod_header_init(&header, sample_rate_hz);
        if (fwrite(&header, sizeof(header), 1U, file) != 1U) {
            err = ESP_FAIL;
        }
    }

    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        if (file != NULL) {
            (void)fclose(file);
        }
        (void)remove(s_lod_path);
        seismic_recorder_lod_store_clear_state();
    }
    else {
        s_lod_file = file;
        seismic_recorder_lod_builder_reset(&s_lod_builder, 0U);
    }

    storage_manager_operation_end();

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "LOD64 sidecar started: %s block_frames=%u",
            s_lod_path,
            (unsigned)SEISMIC_RECORDER_LOD_BLOCK_FRAMES
        );
    }
    return err;
}


esp_err_t seismic_recorder_lod_store_append(
    const seismic_frame_t *persisted_frames,
    size_t frame_count
)
{
    if (persisted_frames == NULL || frame_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lod_file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t consumed = 0U;
    while (consumed < frame_count) {
        const size_t remaining = frame_count - consumed;
        const size_t chunk = remaining < SEISMIC_RECORDER_LOD_APPEND_FRAMES
            ? remaining
            : SEISMIC_RECORDER_LOD_APPEND_FRAMES;

        seismic_recorder_lod_record_t records[
            SEISMIC_RECORDER_LOD_APPEND_RECORDS
        ];
        size_t record_count = 0U;
        const uint64_t first_session_frame = s_lod_builder.next_session_frame;
        esp_err_t err = seismic_recorder_lod_builder_append(
            &s_lod_builder,
            first_session_frame,
            persisted_frames + consumed,
            chunk,
            records,
            SEISMIC_RECORDER_LOD_APPEND_RECORDS,
            &record_count
        );
        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "LOD64 aggregation disabled after validation error: %s",
                esp_err_to_name(err)
            );
            seismic_recorder_lod_store_abort();
            return err;
        }

        if (record_count > 0U) {
            err = storage_manager_operation_begin();
            if (err != ESP_OK) {
                seismic_recorder_lod_store_abort();
                return err;
            }

            const size_t records_written = fwrite(
                records,
                sizeof(records[0]),
                record_count,
                s_lod_file
            );
            const bool write_failed =
                records_written != record_count ||
                ferror(s_lod_file) != 0;
            storage_manager_operation_end();

            if (write_failed) {
                ESP_LOGW(TAG, "LOD64 sidecar write failed; removing index");
                seismic_recorder_lod_store_abort();
                return ESP_FAIL;
            }
        }

        consumed += chunk;
    }

    return ESP_OK;
}


esp_err_t seismic_recorder_lod_store_flush(bool sync_to_media)
{
    if (s_lod_file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    if (fflush(s_lod_file) != 0) {
        err = ESP_FAIL;
    }
    else if (sync_to_media && fsync(fileno(s_lod_file)) != 0) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LOD64 flush failed; removing acceleration index");
        seismic_recorder_lod_store_abort();
    }
    return err;
}


esp_err_t seismic_recorder_lod_store_close(void)
{
    if (s_lod_file == NULL) {
        return ESP_OK;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        FILE *file = s_lod_file;
        seismic_recorder_lod_store_clear_state();
        (void)fclose(file);
        return err;
    }

    FILE *file = s_lod_file;
    s_lod_file = NULL;
    const int close_result = fclose(file);
    if (close_result != 0) {
        err = ESP_FAIL;
        if (s_lod_path[0] != '\0') {
            (void)remove(s_lod_path);
        }
    }
    s_lod_path[0] = '\0';
    seismic_recorder_lod_builder_reset(&s_lod_builder, 0U);

    storage_manager_operation_end();
    return err;
}


void seismic_recorder_lod_store_abort(void)
{
    if (s_lod_file == NULL && s_lod_path[0] == '\0') {
        return;
    }

    const esp_err_t err = storage_manager_operation_begin();
    if (err == ESP_OK) {
        seismic_recorder_lod_store_invalidate_locked();
        storage_manager_operation_end();
        return;
    }

    /* Match the recorder's raw close policy on arbitration failure: never
     * leak a libc stream. Without the storage guard we cannot safely remove
     * the directory entry, but incomplete/error metadata prevents this index
     * from being authoritative and later size/header validation still gates
     * its use. */
    FILE *file = s_lod_file;
    seismic_recorder_lod_store_clear_state();
    if (file != NULL) {
        (void)fclose(file);
    }
}
