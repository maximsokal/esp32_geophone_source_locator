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

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "seismic_recorder_internal.h"
#include "seismic_recorder_lod.h"
#include "storage_manager.h"


#define SEISMIC_RECORDER_LOD_APPEND_FRAMES   256U
#define SEISMIC_RECORDER_LOD_BUFFER_RECORDS   64U
#define SEISMIC_RECORDER_LOD_DMA_RECORDS       8U


static const char *TAG = "SEISMIC_RECORDER_LOD";
static const char *LOD_FILE_NAME = "waveform-lod64.bin";

static bool s_lod_active = false;
static char s_lod_path[SEISMIC_RECORDER_PATH_SIZE];
static uint32_t s_lod_sample_rate_hz = 0U;
static seismic_recorder_lod_builder_t s_lod_builder;
static seismic_recorder_lod_record_t *s_lod_records = NULL;
static size_t s_lod_record_count = 0U;

DMA_ATTR static seismic_recorder_lod_record_t s_lod_dma_staging[
    SEISMIC_RECORDER_LOD_DMA_RECORDS
];


static void seismic_recorder_lod_store_clear_state(void)
{
    if (s_lod_records != NULL) {
        heap_caps_free(s_lod_records);
    }

    s_lod_active = false;
    s_lod_path[0] = '\0';
    s_lod_sample_rate_hz = 0U;
    s_lod_records = NULL;
    s_lod_record_count = 0U;
    seismic_recorder_lod_builder_reset(&s_lod_builder, 0U);
}


static void seismic_recorder_lod_store_invalidate_locked(void)
{
    char path[SEISMIC_RECORDER_PATH_SIZE];
    snprintf(path, sizeof(path), "%s", s_lod_path);
    seismic_recorder_lod_store_clear_state();

    if (path[0] != '\0' && remove(path) != 0) {
        ESP_LOGW(TAG, "Failed to remove invalid LOD sidecar: %s", path);
    }
}


static esp_err_t seismic_recorder_lod_store_write_buffer(
    bool sync_to_media
)
{
    if (!s_lod_active || s_lod_records == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lod_record_count == 0U && !sync_to_media) {
        return ESP_OK;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(s_lod_path, "ab");
    if (file == NULL) {
        err = ESP_FAIL;
    }

    size_t written = 0U;
    while (err == ESP_OK && written < s_lod_record_count) {
        const size_t remaining = s_lod_record_count - written;
        const size_t chunk = remaining < SEISMIC_RECORDER_LOD_DMA_RECORDS
            ? remaining
            : SEISMIC_RECORDER_LOD_DMA_RECORDS;

        memcpy(
            s_lod_dma_staging,
            s_lod_records + written,
            chunk * sizeof(s_lod_dma_staging[0])
        );

        const size_t chunk_written = fwrite(
            s_lod_dma_staging,
            sizeof(s_lod_dma_staging[0]),
            chunk,
            file
        );
        if (chunk_written != chunk || ferror(file) != 0) {
            err = ESP_FAIL;
            break;
        }
        written += chunk_written;
    }

    if (err == ESP_OK && file != NULL && fflush(file) != 0) {
        err = ESP_FAIL;
    }
    if (
        err == ESP_OK &&
        sync_to_media &&
        file != NULL &&
        fsync(fileno(file)) != 0
    ) {
        err = ESP_FAIL;
    }
    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();

    if (err == ESP_OK) {
        s_lod_record_count = 0U;
    }
    return err;
}


static esp_err_t seismic_recorder_lod_store_mark_complete(void)
{
    if (!s_lod_active || s_lod_sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        return err;
    }

    FILE *file = fopen(s_lod_path, "rb+");
    if (file == NULL) {
        err = ESP_FAIL;
    }

    seismic_recorder_lod_file_header_t header;
    if (err == ESP_OK) {
        seismic_recorder_lod_header_init(&header, s_lod_sample_rate_hz);
        seismic_recorder_lod_header_mark_complete(&header);
        if (fseek(file, 0L, SEEK_SET) != 0) {
            err = ESP_FAIL;
        }
    }
    if (
        err == ESP_OK &&
        fwrite(&header, sizeof(header), 1U, file) != 1U
    ) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fsync(fileno(file)) != 0) {
        err = ESP_FAIL;
    }
    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    return err;
}


bool seismic_recorder_lod_store_is_active(void)
{
    return s_lod_active;
}


esp_err_t seismic_recorder_lod_store_begin(
    const char *session_path,
    uint32_t sample_rate_hz
)
{
    if (session_path == NULL || sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lod_active || s_lod_records != NULL || s_lod_path[0] != '\0') {
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

    s_lod_records = heap_caps_malloc(
        sizeof(*s_lod_records) * SEISMIC_RECORDER_LOD_BUFFER_RECORDS,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_lod_records == NULL) {
        seismic_recorder_lod_store_clear_state();
        return ESP_ERR_NO_MEM;
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
    if (file != NULL && fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        (void)remove(s_lod_path);
    }

    storage_manager_operation_end();

    if (err != ESP_OK) {
        seismic_recorder_lod_store_clear_state();
        return err;
    }

    s_lod_active = true;
    s_lod_sample_rate_hz = sample_rate_hz;
    s_lod_record_count = 0U;
    seismic_recorder_lod_builder_reset(&s_lod_builder, 0U);

    ESP_LOGI(
        TAG,
        "LOD64 sidecar started: %s block_frames=%u buffer_records=%u",
        s_lod_path,
        (unsigned)SEISMIC_RECORDER_LOD_BLOCK_FRAMES,
        (unsigned)SEISMIC_RECORDER_LOD_BUFFER_RECORDS
    );
    return ESP_OK;
}


esp_err_t seismic_recorder_lod_store_append(
    const seismic_frame_t *persisted_frames,
    size_t frame_count
)
{
    if (persisted_frames == NULL || frame_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lod_active || s_lod_records == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t consumed = 0U;
    while (consumed < frame_count) {
        const size_t remaining = frame_count - consumed;
        const size_t chunk = remaining < SEISMIC_RECORDER_LOD_APPEND_FRAMES
            ? remaining
            : SEISMIC_RECORDER_LOD_APPEND_FRAMES;
        const size_t emitted =
            (s_lod_builder.pending_count + chunk) /
            SEISMIC_RECORDER_LOD_BLOCK_FRAMES;

        if (
            emitted >
            SEISMIC_RECORDER_LOD_BUFFER_RECORDS - s_lod_record_count
        ) {
            esp_err_t flush_err =
                seismic_recorder_lod_store_write_buffer(false);
            if (flush_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "LOD64 buffered write failed; disabling index: %s",
                    esp_err_to_name(flush_err)
                );
                seismic_recorder_lod_store_abort();
                return flush_err;
            }
        }

        size_t emitted_now = 0U;
        const uint64_t first_session_frame = s_lod_builder.next_session_frame;
        esp_err_t err = seismic_recorder_lod_builder_append(
            &s_lod_builder,
            first_session_frame,
            persisted_frames + consumed,
            chunk,
            s_lod_records + s_lod_record_count,
            SEISMIC_RECORDER_LOD_BUFFER_RECORDS - s_lod_record_count,
            &emitted_now
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

        s_lod_record_count += emitted_now;
        consumed += chunk;

        if (s_lod_record_count == SEISMIC_RECORDER_LOD_BUFFER_RECORDS) {
            err = seismic_recorder_lod_store_write_buffer(false);
            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "LOD64 buffered write failed; disabling index: %s",
                    esp_err_to_name(err)
                );
                seismic_recorder_lod_store_abort();
                return err;
            }
        }
    }

    return ESP_OK;
}


esp_err_t seismic_recorder_lod_store_flush(bool sync_to_media)
{
    if (!s_lod_active) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err =
        seismic_recorder_lod_store_write_buffer(sync_to_media);
    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "LOD64 flush failed; disabling acceleration index: %s",
            esp_err_to_name(err)
        );
        seismic_recorder_lod_store_abort();
    }
    return err;
}


esp_err_t seismic_recorder_lod_store_close(void)
{
    if (!s_lod_active) {
        return ESP_OK;
    }

    esp_err_t err = seismic_recorder_lod_store_write_buffer(true);
    if (err == ESP_OK) {
        err = seismic_recorder_lod_store_mark_complete();
    }

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "LOD64 finalization failed; removing acceleration index: %s",
            esp_err_to_name(err)
        );
        seismic_recorder_lod_store_abort();
        return err;
    }

    ESP_LOGI(
        TAG,
        "LOD64 sidecar complete: session_frames=%llu records=%llu",
        (unsigned long long)s_lod_builder.next_session_frame,
        (unsigned long long)(
            s_lod_builder.next_session_frame /
            SEISMIC_RECORDER_LOD_BLOCK_FRAMES
        )
    );
    seismic_recorder_lod_store_clear_state();
    return ESP_OK;
}


void seismic_recorder_lod_store_abort(void)
{
    if (!s_lod_active && s_lod_records == NULL && s_lod_path[0] == '\0') {
        return;
    }

    const esp_err_t err = storage_manager_operation_begin();
    if (err == ESP_OK) {
        seismic_recorder_lod_store_invalidate_locked();
        storage_manager_operation_end();
        return;
    }

    /* The on-disk header remains incomplete until close() marks it complete.
     * If storage arbitration itself is unavailable we can therefore safely
     * release RAM state even when the partial file cannot be removed now: a
     * future reader must reject that header and fall back to samples.bin. */
    seismic_recorder_lod_store_clear_state();
}
