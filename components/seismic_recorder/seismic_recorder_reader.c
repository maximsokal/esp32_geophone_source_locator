#include "seismic_recorder_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "seismic_recorder_lod_reader.h"
#include "storage_manager.h"

#ifdef ESP_PLATFORM
#include "app_task_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif


#define SEISMIC_RECORDER_LOD_READ_STAGING_RECORDS 32U


typedef struct
{
    const char *path;

} seismic_recorder_bounded_reader_t;


typedef struct
{
    const char *object_id;
    const char *session_id;
    uint32_t sample_rate_hz;

} seismic_recorder_cache_reader_t;


#ifdef ESP_PLATFORM
static const char *TAG = "SEISMIC_RECORDER_READER";


typedef struct
{
    UBaseType_t original_priority;
    bool changed;

} seismic_recorder_reader_priority_guard_t;


static void seismic_recorder_reader_log_path(
    const char *source,
    const char *session_id,
    uint64_t start_frame,
    uint64_t frame_count,
    size_t point_count,
    const seismic_recorder_lod_read_stats_t *lod_stats
)
{
    const uint64_t raw_frames =
        lod_stats != NULL ? lod_stats->raw_frames_read : 0U;
    const uint64_t lod_records =
        lod_stats != NULL ? lod_stats->lod_records_read : 0U;
    const uint64_t lod_frames =
        lod_stats != NULL ? lod_stats->lod_frames_read : 0U;

    ESP_LOGI(
        TAG,
        "WAVEFORM_READ_PATH source=%s session=%s start=%llu frames=%llu points=%u raw_frames=%llu lod_records=%llu lod_frames=%llu",
        source,
        session_id,
        (unsigned long long)start_frame,
        (unsigned long long)frame_count,
        (unsigned)point_count,
        (unsigned long long)raw_frames,
        (unsigned long long)lod_records,
        (unsigned long long)lod_frames
    );
}


static seismic_recorder_reader_priority_guard_t
seismic_recorder_reader_priority_enter(void)
{
    seismic_recorder_reader_priority_guard_t guard = {
        .original_priority = uxTaskPriorityGet(NULL),
        .changed = false,
    };

    /*
     * ESP-IDF HTTPD normally runs above the recorder and LVGL. A long saved
     * waveform request can therefore release one bounded SD window and then
     * immediately reacquire it for the next chunk before the recorder gets
     * CPU time. Lower only the range-reduction phase to the normal storage
     * service priority. This keeps each SD window bounded *and* lets the
     * recorder time-slice between chunks while LVGL can still preempt both.
     * Small metadata checks and normal HTTPS handling retain HTTPD priority.
     */
    if (guard.original_priority > APP_TASK_PRIORITY_STORAGE_SERVICE) {
        vTaskPrioritySet(NULL, APP_TASK_PRIORITY_STORAGE_SERVICE);
        guard.changed = true;
    }

    return guard;
}


static void seismic_recorder_reader_priority_leave(
    seismic_recorder_reader_priority_guard_t guard
)
{
    if (guard.changed) {
        vTaskPrioritySet(NULL, guard.original_priority);
    }
}
#endif


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


static esp_err_t seismic_recorder_cache_reader_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    seismic_recorder_cache_reader_t *reader =
        (seismic_recorder_cache_reader_t *)context;

    if (
        reader == NULL ||
        reader->object_id == NULL ||
        reader->session_id == NULL ||
        buffer == NULL ||
        size_bytes == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (offset_bytes == 0U) {
        if (size_bytes != sizeof(seismic_recorder_binary_header_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        seismic_recorder_binary_header_t header = {
            .magic = {'G', 'E', 'O', 'S', 'E', 'I', 'S', '1'},
            .version = SEISMIC_RECORDER_BINARY_VERSION,
            .channel_count = SEISMIC_CHANNEL_COUNT,
            .sample_rate_hz = reader->sample_rate_hz,
            .frame_size_bytes = sizeof(seismic_frame_t),
            .reserved = 0U,
        };
        memcpy(buffer, &header, sizeof(header));
        return ESP_OK;
    }

    if (offset_bytes < sizeof(seismic_recorder_binary_header_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t payload_offset =
        offset_bytes - sizeof(seismic_recorder_binary_header_t);
    if (
        payload_offset % sizeof(seismic_frame_t) != 0U ||
        size_bytes % sizeof(seismic_frame_t) != 0U
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t first_session_frame =
        payload_offset / sizeof(seismic_frame_t);
    const size_t frame_count =
        size_bytes / sizeof(seismic_frame_t);
    if (frame_count == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    return seismic_recorder_cache_copy_session_range(
        reader->object_id,
        reader->session_id,
        first_session_frame,
        (seismic_frame_t *)buffer,
        frame_count
    );
}


static bool seismic_recorder_reader_resolve_range(
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    uint64_t requested_frame_count,
    uint64_t *resolved_frame_count
)
{
    if (summary == NULL || resolved_frame_count == NULL) {
        return false;
    }
    if (start_frame > summary->samples_written) {
        return false;
    }

    const uint64_t available = summary->samples_written - start_frame;
    const uint64_t resolved =
        requested_frame_count == 0U
            ? available
            : requested_frame_count;
    if (resolved > available) {
        return false;
    }

    *resolved_frame_count = resolved;
    return true;
}


static bool seismic_recorder_reader_virtual_file_size(
    uint64_t frame_count,
    uint64_t *file_size
)
{
    if (file_size == NULL) {
        return false;
    }
    if (
        frame_count >
            (UINT64_MAX - sizeof(seismic_recorder_binary_header_t)) /
            sizeof(seismic_frame_t)
    ) {
        return false;
    }

    *file_size =
        sizeof(seismic_recorder_binary_header_t) +
        frame_count * sizeof(seismic_frame_t);
    return true;
}


static esp_err_t seismic_recorder_reader_append_suffix(
    char *path,
    size_t path_size,
    const char *suffix
)
{
    if (path == NULL || path_size == 0U || suffix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    if (path_length + suffix_length >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(path + path_length, suffix, suffix_length + 1U);
    return ESP_OK;
}


static bool seismic_recorder_reader_header_matches_summary(
    const seismic_recorder_binary_header_t *header,
    const seismic_recorder_session_summary_t *summary
)
{
    return
        header != NULL &&
        summary != NULL &&
        memcmp(header->magic, "GEOSEIS1", sizeof(header->magic)) == 0 &&
        header->version == SEISMIC_RECORDER_BINARY_VERSION &&
        header->channel_count == SEISMIC_CHANNEL_COUNT &&
        header->channel_count == summary->channel_count &&
        header->sample_rate_hz > 0U &&
        header->sample_rate_hz == summary->sample_rate_hz &&
        header->frame_size_bytes == sizeof(seismic_frame_t) &&
        header->reserved == 0U;
}


static void seismic_recorder_reader_apply_terminal_info(
    const seismic_recorder_session_summary_t *summary,
    seismic_recorder_waveform_info_t *info
)
{
    info->status = summary->status;
    info->complete = summary->complete;
    /* Legacy in-memory fixtures and pre-MAX-69 callers may only populate the
     * historic complete bit. Persisted metadata parsing sets readable
     * explicitly, but complete has always implied a readable terminal file. */
    info->readable = summary->readable || summary->complete;
    info->recovered = summary->recovered;
    info->dropped_frames_available = summary->dropped_frames_available;
    info->recording_ms_available = summary->recording_ms_available;
    info->samples_written = summary->samples_written;
    info->dropped_frames = summary->dropped_frames;
    info->recording_ms = summary->recording_ms;
    info->cadence = summary->cadence;
}


static void seismic_recorder_reader_fill_empty_info(
    const char *object_id,
    const seismic_recorder_session_summary_t *summary,
    uint64_t start_frame,
    seismic_recorder_waveform_info_t *info
)
{
    seismic_recorder_reader_apply_terminal_info(summary, info);
    snprintf(info->object_id, sizeof(info->object_id), "%s", object_id);
    snprintf(info->session_id, sizeof(info->session_id), "%s", summary->session_id);
    snprintf(info->source_name, sizeof(info->source_name), "%s", summary->source_name);
    info->sample_rate_hz = summary->sample_rate_hz;
    info->channel_count = summary->channel_count;
    info->source_frame_count = summary->samples_written;
    info->start_frame = start_frame;
    info->frame_count = 0U;
    info->point_count = 0U;
}


static bool seismic_recorder_reader_lod_worthwhile(
    uint64_t frame_count,
    size_t max_points
)
{
    const size_t point_count =
        frame_count < (uint64_t)max_points
            ? (size_t)frame_count
            : max_points;

    return
        point_count > 0U &&
        frame_count >=
            (uint64_t)point_count * SEISMIC_RECORDER_LOD_BLOCK_FRAMES;
}


/*
 * Build an optional LOD path without ever contaminating the authoritative
 * samples.bin error state. A custom mount point or future identifier format
 * can make the longer sidecar suffix exceed SEISMIC_RECORDER_PATH_SIZE even
 * while /samples.bin still fits. That condition must mean "no acceleration",
 * not "saved waveform unavailable".
 */
static char *seismic_recorder_reader_try_lod_path(
    const char *session_path,
    uint64_t frame_count,
    size_t max_points
)
{
    if (
        session_path == NULL ||
        !seismic_recorder_reader_lod_worthwhile(frame_count, max_points)
    ) {
        return NULL;
    }

    char *lod_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (lod_path == NULL) {
        return NULL;
    }

    const int written = snprintf(
        lod_path,
        SEISMIC_RECORDER_PATH_SIZE,
        "%s",
        session_path
    );
    if (written < 0 || written >= SEISMIC_RECORDER_PATH_SIZE) {
        free(lod_path);
        return NULL;
    }

    const esp_err_t path_err = seismic_recorder_reader_append_suffix(
        lod_path,
        SEISMIC_RECORDER_PATH_SIZE,
        "/" SEISMIC_RECORDER_LOD_FILE_NAME
    );
    if (path_err != ESP_OK) {
        free(lod_path);
        return NULL;
    }

    return lod_path;
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

    if (!summary.readable && !summary.complete) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t resolved_frame_count = 0U;
    if (!seismic_recorder_reader_resolve_range(
            &summary,
            start_frame,
            frame_count,
            &resolved_frame_count
        )) {
        return ESP_ERR_INVALID_ARG;
    }

    char *samples_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (samples_path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = seismic_recorder_fs_build_session_path(
        object_id,
        session_id,
        samples_path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    char *lod_path = NULL;
    if (err == ESP_OK) {
        lod_path = seismic_recorder_reader_try_lod_path(
            samples_path,
            resolved_frame_count,
            max_points
        );
    }

    if (err == ESP_OK) {
        err = seismic_recorder_reader_append_suffix(
            samples_path,
            SEISMIC_RECORDER_PATH_SIZE,
            "/samples.bin"
        );
    }
    if (err != ESP_OK) {
        free(lod_path);
        free(samples_path);
        return err;
    }

    uint64_t raw_file_size = 0U;
    err = seismic_recorder_reader_file_size(
        samples_path,
        &raw_file_size
    );
    if (err != ESP_OK) {
        free(lod_path);
        free(samples_path);
        return err;
    }

    uint64_t expected_raw_file_size = 0U;
    if (!seismic_recorder_reader_virtual_file_size(
            summary.samples_written,
            &expected_raw_file_size
        )) {
        free(lod_path);
        free(samples_path);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Final ERROR metadata is the authoritative committed prefix. A failed
     * fwrite can leave bytes after its last fully-accounted frame; expose only
     * the exact metadata prefix rather than turning the whole failed run into
     * an unreadable session. COMPLETE and RECOVERED_ERROR remain exact-size
     * contracts, and every status fails closed if the raw file is too short. */
    const bool terminal_error_prefix =
        summary.status == SEISMIC_RECORDER_SESSION_ERROR &&
        !summary.recovered;
    if (
        raw_file_size < expected_raw_file_size ||
        (!terminal_error_prefix && raw_file_size != expected_raw_file_size)
    ) {
        free(lod_path);
        free(samples_path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint64_t contract_raw_file_size = expected_raw_file_size;

    seismic_recorder_bounded_reader_t raw_reader = {
        .path = samples_path,
    };
    seismic_recorder_binary_header_t raw_header;
    err = seismic_recorder_reader_read_at(
        &raw_reader,
        0U,
        &raw_header,
        sizeof(raw_header)
    );
    if (err != ESP_OK) {
        free(lod_path);
        free(samples_path);
        return err;
    }
    if (!seismic_recorder_reader_header_matches_summary(&raw_header, &summary)) {
        free(lod_path);
        free(samples_path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * Terminal metadata is the frame-count authority. Validate samples.bin
     * before cache/LOD selection so a retained PSRAM range cannot hide a
     * truncated or configuration-mismatched persisted session. ERROR may have
     * a discarded physical tail, but it is never part of this logical file.
     */
    if (resolved_frame_count == 0U) {
        seismic_recorder_reader_fill_empty_info(
            object_id,
            &summary,
            start_frame,
            info
        );
        free(lod_path);
        free(samples_path);
        return ESP_OK;
    }

    seismic_frame_t *staging = heap_caps_malloc(
        sizeof(*staging) * SEISMIC_RECORDER_READ_STAGING_FRAMES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (staging == NULL) {
        free(lod_path);
        free(samples_path);
        return ESP_ERR_NO_MEM;
    }

    /*
     * Fast path: after STOP the newest retained range can still be in the
     * recorder PSRAM cache. Reading it avoids bulk TFT/SD arbitration. The
     * persisted file/header contract was already checked above, and partial
     * cache coverage never yields a mixed waveform.
     */
    uint64_t virtual_file_size = 0U;
    const bool virtual_size_valid =
        seismic_recorder_reader_virtual_file_size(
            summary.samples_written,
            &virtual_file_size
        );
    if (
        virtual_size_valid &&
        seismic_recorder_cache_contains_session_range(
            object_id,
            session_id,
            start_frame,
            resolved_frame_count
        )
    ) {
        seismic_recorder_cache_reader_t cache_reader = {
            .object_id = object_id,
            .session_id = session_id,
            .sample_rate_hz = summary.sample_rate_hz,
        };
#ifdef ESP_PLATFORM
        const seismic_recorder_reader_priority_guard_t priority_guard =
            seismic_recorder_reader_priority_enter();
#endif
        err = seismic_recorder_waveform_read_source(
            seismic_recorder_cache_reader_read_at,
            &cache_reader,
            virtual_file_size,
            &summary,
            start_frame,
            resolved_frame_count,
            max_points,
            staging,
            SEISMIC_RECORDER_READ_STAGING_FRAMES,
            points,
            point_capacity,
            info
        );
#ifdef ESP_PLATFORM
        seismic_recorder_reader_priority_leave(priority_guard);
#endif
        if (err == ESP_OK) {
            seismic_recorder_reader_apply_terminal_info(&summary, info);
            heap_caps_free(staging);
            free(lod_path);
            free(samples_path);
            snprintf(info->object_id, sizeof(info->object_id), "%s", object_id);
#ifdef ESP_PLATFORM
            seismic_recorder_reader_log_path(
                "cache",
                session_id,
                start_frame,
                resolved_frame_count,
                info->point_count,
                NULL
            );
#endif
            return ESP_OK;
        }
        /* A new recording can reset the retained cache between the range probe
         * and a chunk copy. Fall back to authoritative SD rather than exposing
         * a transient cache miss as session corruption. */
        memset(info, 0, sizeof(*info));
    }

    /*
     * CONFIG_FATFS_FS_LOCK=0 means an unlocked long-lived FILE * cannot be
     * made safe against concurrent object/session mutation by libc itself.
     * Keep no handle across arbitration windows: every read_at callback opens,
     * seeks, reads one bounded chunk, closes, and only then releases SPI2.
     *
     * For long ranges, a completed LOD64 sidecar can replace interior raw
     * blocks only when all represented frames map to exactly one output bin.
     * Error/recovered sessions deliberately fail the LOD complete-session gate
     * and fall through to authoritative raw samples.bin.
     *
     * The reduction phase is temporarily lowered to the normal storage-service
     * priority so a many-window history request cannot outrank recorder/LVGL.
     */
    bool hybrid_completed = false;
    if (lod_path != NULL) {
        uint64_t lod_file_size = 0U;
        if (
            seismic_recorder_reader_file_size(
                lod_path,
                &lod_file_size
            ) == ESP_OK
        ) {
            seismic_recorder_lod_record_t *lod_staging = heap_caps_malloc(
                sizeof(*lod_staging) *
                    SEISMIC_RECORDER_LOD_READ_STAGING_RECORDS,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );
            if (lod_staging != NULL) {
                seismic_recorder_bounded_reader_t lod_reader = {
                    .path = lod_path,
                };
                seismic_recorder_lod_read_stats_t lod_stats = {0};
#ifdef ESP_PLATFORM
                const seismic_recorder_reader_priority_guard_t priority_guard =
                    seismic_recorder_reader_priority_enter();
#endif
                const esp_err_t lod_err =
                    seismic_recorder_lod_read_source_with_stats(
                        seismic_recorder_reader_read_at,
                        &raw_reader,
                        contract_raw_file_size,
                        seismic_recorder_reader_read_at,
                        &lod_reader,
                        lod_file_size,
                        &summary,
                        start_frame,
                        resolved_frame_count,
                        max_points,
                        staging,
                        SEISMIC_RECORDER_READ_STAGING_FRAMES,
                        lod_staging,
                        SEISMIC_RECORDER_LOD_READ_STAGING_RECORDS,
                        points,
                        point_capacity,
                        info,
                        &lod_stats
                    );
#ifdef ESP_PLATFORM
                seismic_recorder_reader_priority_leave(priority_guard);
#endif
                heap_caps_free(lod_staging);

                if (lod_err == ESP_OK) {
                    hybrid_completed = true;
                    err = ESP_OK;
#ifdef ESP_PLATFORM
                    seismic_recorder_reader_log_path(
                        lod_stats.lod_records_read > 0U ? "lod64" : "raw",
                        session_id,
                        start_frame,
                        resolved_frame_count,
                        info->point_count,
                        &lod_stats
                    );
#endif
                }
                else {
                    memset(info, 0, sizeof(*info));
                }
            }
        }
    }

    if (!hybrid_completed) {
#ifdef ESP_PLATFORM
        const seismic_recorder_reader_priority_guard_t priority_guard =
            seismic_recorder_reader_priority_enter();
#endif
        err = seismic_recorder_waveform_read_source(
            seismic_recorder_reader_read_at,
            &raw_reader,
            contract_raw_file_size,
            &summary,
            start_frame,
            resolved_frame_count,
            max_points,
            staging,
            SEISMIC_RECORDER_READ_STAGING_FRAMES,
            points,
            point_capacity,
            info
        );
#ifdef ESP_PLATFORM
        seismic_recorder_reader_priority_leave(priority_guard);
        if (err == ESP_OK) {
            seismic_recorder_reader_log_path(
                "raw",
                session_id,
                start_frame,
                resolved_frame_count,
                info->point_count,
                NULL
            );
        }
#endif
    }

    heap_caps_free(staging);
    free(lod_path);
    free(samples_path);

    if (err == ESP_OK) {
        seismic_recorder_reader_apply_terminal_info(&summary, info);
        snprintf(info->object_id, sizeof(info->object_id), "%s", object_id);
    }
    return err;
}
