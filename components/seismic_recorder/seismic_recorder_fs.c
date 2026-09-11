#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

#include "object_format.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage_manager.h"


static const char *TAG = "SEISMIC_RECORDER_FS";


typedef enum
{
    SEISMIC_RECORDER_FS_METADATA_NONE = 0,
    SEISMIC_RECORDER_FS_METADATA_PRIMARY,
    SEISMIC_RECORDER_FS_METADATA_BACKUP,

} seismic_recorder_fs_metadata_state_t;

/*
 * ESP-IDF 5.5.4's SDSPI path copies external-RAM sources into a temporary
 * 512-byte DMA buffer and sends one block at a time. Keep the large recorder
 * backlog in PSRAM, but make the actual fwrite source internal and DMA-safe.
 * 256 frames are 6144 bytes (12 sectors) for the current 24-byte frame.
 */
DMA_ATTR static seismic_frame_t s_dma_staging[
    SEISMIC_RECORDER_WRITE_STAGING_FRAMES
];

static bool seismic_recorder_fs_parse_session_number(
    const char *name,
    unsigned *number
);

static esp_err_t seismic_recorder_fs_session_metadata_state(
    const char *sessions_path,
    const char *session_id,
    char *session_path,
    size_t session_path_size,
    seismic_recorder_fs_metadata_state_t *metadata_state
);


static esp_err_t seismic_recorder_fs_sessions_path(
    const char *object_id,
    char *path,
    size_t path_size
)
{
    if (!object_format_id_is_valid(object_id)) {
        return ESP_ERR_INVALID_ARG;
    }

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


esp_err_t seismic_recorder_fs_build_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    unsigned session_number = 0U;
    if (
        !object_format_id_is_valid(object_id) ||
        !seismic_recorder_fs_parse_session_number(session_id, &session_number)
    ) {
        return ESP_ERR_INVALID_ARG;
    }

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


static esp_err_t seismic_recorder_fs_errno_to_error(
    int error_number
)
{
    if (error_number == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }

    if (error_number == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_FAIL;
}


static esp_err_t seismic_recorder_fs_session_metadata_state(
    const char *sessions_path,
    const char *session_id,
    char *session_path,
    size_t session_path_size,
    seismic_recorder_fs_metadata_state_t *metadata_state
)
{
    if (
        sessions_path == NULL ||
        session_id == NULL ||
        session_path == NULL ||
        session_path_size == 0U ||
        metadata_state == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *metadata_state = SEISMIC_RECORDER_FS_METADATA_NONE;

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
        const int directory_errno = errno;

        /* A directory entry can disappear between the parent census and
         * this child open. Treat that as an incomplete session, but surface
         * other filesystem failures instead of silently hiding them. */
        if (
            directory_errno == ENOENT ||
            directory_errno == ENOTDIR
        ) {
            return ESP_OK;
        }

        return seismic_recorder_fs_errno_to_error(directory_errno);
    }

    errno = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, "session.json") == 0) {
            *metadata_state = SEISMIC_RECORDER_FS_METADATA_PRIMARY;
            break;
        }

        if (strcmp(entry->d_name, "session.json.bak") == 0) {
            *metadata_state = SEISMIC_RECORDER_FS_METADATA_BACKUP;
        }
    }

    const int read_errno = errno;
    const int close_result = closedir(directory);
    if (read_errno != 0) {
        return seismic_recorder_fs_errno_to_error(read_errno);
    }

    if (close_result != 0) {
        return seismic_recorder_fs_errno_to_error(errno);
    }

    return ESP_OK;
}


static esp_err_t seismic_recorder_fs_next_session_id(
    const char *object_id,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE]
)
{
    char *sessions_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (sessions_path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = seismic_recorder_fs_sessions_path(
        object_id,
        sessions_path,
        SEISMIC_RECORDER_PATH_SIZE
    );
    if (err != ESP_OK) {
        free(sessions_path);
        return err;
    }

    DIR *directory = opendir(sessions_path);
    if (directory == NULL) {
        free(sessions_path);
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
    free(sessions_path);

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
    session_id[0] = '\0';

    const int64_t operation_started_us =
        esp_timer_get_time();

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    err = seismic_recorder_fs_next_session_id(
        object_id,
        session_id
    );

    if (err == ESP_OK) {
        err = seismic_recorder_fs_build_session_path(
            object_id,
            session_id,
            path,
            SEISMIC_RECORDER_PATH_SIZE
        );
    }

    if (err == ESP_OK) {
        err = storage_manager_create_directory(path);
    }

    if (err == ESP_OK) {
        static const char samples_suffix[] = "/samples.bin";
        const size_t path_length = strlen(path);
        const size_t suffix_length = sizeof(samples_suffix) - 1U;
        if (path_length + suffix_length >= SEISMIC_RECORDER_PATH_SIZE) {
            err = ESP_ERR_INVALID_SIZE;
        }
        else {
            memcpy(path + path_length, samples_suffix, suffix_length + 1U);
        }
    }

    FILE *file = NULL;
    if (err == ESP_OK) {
        file = fopen(path, "wb");
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
            if (
                seismic_recorder_fs_build_session_path(
                    object_id,
                    session_id,
                    path,
                    SEISMIC_RECORDER_PATH_SIZE
                ) == ESP_OK
            ) {
                (void)storage_manager_remove_tree(path);
            }
        }
    }
    else {
        *samples_file = file;
    }

    storage_manager_operation_end();
    free(path);
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=create-session result=%d elapsed_us=%lld",
        (int)err,
        (long long)(esp_timer_get_time() - operation_started_us)
    );
    (void)operation_started_us;
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

    const int64_t operation_started_us = esp_timer_get_time();
    const int64_t fwrite_started_us = operation_started_us;
    esp_err_t err = ESP_OK;
    size_t written = 0U;
    unsigned storage_windows = 0U;
    int64_t max_storage_window_us = 0;
    while (written < frame_count) {
        const size_t remaining = frame_count - written;
        const size_t chunk = remaining < SEISMIC_RECORDER_WRITE_STAGING_FRAMES
            ? remaining
            : SEISMIC_RECORDER_WRITE_STAGING_FRAMES;

        memcpy(
            s_dma_staging,
            frames + written,
            chunk * sizeof(seismic_frame_t)
        );

        /* Release the LVGL/SPI2 guard between bounded writes. A slow SD
         * card must not hold the TFT mutex for the whole recorder batch. */
        const int64_t window_started_us = esp_timer_get_time();
        err = storage_manager_operation_begin();
        if (err != ESP_OK) {
            break;
        }

        const size_t chunk_written = fwrite(
            s_dma_staging,
            sizeof(seismic_frame_t),
            chunk,
            samples_file
        );
        written += chunk_written;
        storage_manager_operation_end();
        ++storage_windows;

        const int64_t storage_window_us =
            esp_timer_get_time() - window_started_us;
        if (storage_window_us > max_storage_window_us) {
            max_storage_window_us = storage_window_us;
        }

        if (chunk_written != chunk || ferror(samples_file) != 0) {
            err = ESP_FAIL;
            break;
        }
    }

    *frames_written = written;
    if (
        err == ESP_OK &&
        (written != frame_count || ferror(samples_file) != 0)
    ) {
        err = ESP_FAIL;
    }

    const int64_t fwrite_us =
        esp_timer_get_time() - fwrite_started_us;

    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=fwrite requested=%u written=%u windows=%u max_window_us=%lld fwrite_us=%lld operation_us=%lld result=%d",
        (unsigned)frame_count,
        (unsigned)written,
        storage_windows,
        (long long)max_storage_window_us,
        (long long)fwrite_us,
        (long long)(esp_timer_get_time() - operation_started_us),
        (int)err
    );
    (void)operation_started_us;
    (void)fwrite_us;
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

    const int64_t operation_started_us =
        esp_timer_get_time();
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        ESP_LOGI(
            TAG,
            "RECORDER_IO_TIMING op=flush sync=%u operation_us=%lld result=%d",
            sync_to_media ? 1U : 0U,
            (long long)(esp_timer_get_time() - operation_started_us),
            (int)err
        );
        return err;
    }

    const int64_t fflush_started_us =
        esp_timer_get_time();
    int64_t fsync_started_us = 0;
    const int fflush_result = fflush(samples_file);
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=fflush sync=%u elapsed_us=%lld result=%d",
        sync_to_media ? 1U : 0U,
        (long long)(esp_timer_get_time() - fflush_started_us),
        fflush_result
    );
    if (fflush_result != 0) {
        err = ESP_FAIL;
    }
    else if (sync_to_media) {
        fsync_started_us = esp_timer_get_time();
        const int fsync_result = fsync(fileno(samples_file));
        ESP_LOGI(
            TAG,
            "RECORDER_IO_TIMING op=fsync elapsed_us=%lld result=%d",
            (long long)(esp_timer_get_time() - fsync_started_us),
            fsync_result
        );
        if (fsync_result != 0) {
            err = ESP_FAIL;
        }
    }

    storage_manager_operation_end();
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=flush sync=%u operation_us=%lld result=%d",
        sync_to_media ? 1U : 0U,
        (long long)(esp_timer_get_time() - operation_started_us),
        (int)err
    );
    (void)operation_started_us;
    (void)fflush_started_us;
    if (sync_to_media) {
        (void)fsync_started_us;
    }
    return err;
}


esp_err_t seismic_recorder_fs_sync(
    FILE *samples_file
)
{
    if (samples_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t operation_started_us =
        esp_timer_get_time();
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        ESP_LOGI(
            TAG,
            "RECORDER_IO_TIMING op=fsync operation_us=%lld result=%d",
            (long long)(esp_timer_get_time() - operation_started_us),
            (int)err
        );
        return err;
    }

    const int64_t fsync_started_us = esp_timer_get_time();
    const int fsync_result = fsync(fileno(samples_file));
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=fsync elapsed_us=%lld result=%d",
        (long long)(esp_timer_get_time() - fsync_started_us),
        fsync_result
    );
    if (fsync_result != 0) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=fsync operation_us=%lld result=%d",
        (long long)(esp_timer_get_time() - operation_started_us),
        (int)err
    );
    (void)operation_started_us;
    (void)fsync_started_us;
    return err;
}


esp_err_t seismic_recorder_fs_close(
    FILE *samples_file
)
{
    if (samples_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t operation_started_us =
        esp_timer_get_time();
    esp_err_t err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        /*
         * The storage arbitration failed, so the file cannot be flushed or
         * synced safely. Still release the libc FILE handle on this error
         * path; the caller must never lose ownership of an open stream.
         */
        const int forced_fclose_result = fclose(samples_file);
        ESP_LOGI(
            TAG,
            "RECORDER_IO_TIMING op=close operation_us=%lld result=%d forced_fclose=%d",
            (long long)(esp_timer_get_time() - operation_started_us),
            (int)err,
            forced_fclose_result
        );
        (void)forced_fclose_result;
        return err;
    }

    const int64_t fclose_started_us =
        esp_timer_get_time();
    const int fclose_result = fclose(samples_file);
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=fclose elapsed_us=%lld result=%d",
        (long long)(esp_timer_get_time() - fclose_started_us),
        fclose_result
    );
    if (fclose_result != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    storage_manager_operation_end();
    ESP_LOGI(
        TAG,
        "RECORDER_IO_TIMING op=close operation_us=%lld result=%d",
        (long long)(esp_timer_get_time() - operation_started_us),
        (int)err
    );
    (void)operation_started_us;
    (void)fclose_started_us;
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
    if (sessions_path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *session_path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (session_path == NULL) {
        free(sessions_path);
        return ESP_ERR_NO_MEM;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(session_path);
        free(sessions_path);
        return err;
    }

    err = seismic_recorder_fs_sessions_path(
        object_id,
        sessions_path,
        SEISMIC_RECORDER_PATH_SIZE
    );

    DIR *directory = NULL;
    if (err == ESP_OK) {
        directory = opendir(sessions_path);
        if (directory == NULL) {
            err = seismic_recorder_fs_errno_to_error(errno);
        }
    }

    if (err == ESP_OK) {
        struct dirent *entry = NULL;
        while (*session_count < max_sessions) {
            errno = 0;
            entry = readdir(directory);
            if (entry == NULL) {
                if (errno != 0) {
                    err = seismic_recorder_fs_errno_to_error(errno);
                }
                break;
            }

            unsigned number = 0U;
            if (!seismic_recorder_fs_parse_session_number(entry->d_name, &number)) {
                continue;
            }

            seismic_recorder_session_summary_t summary;
            memset(&summary, 0, sizeof(summary));

            /*
             * The directory census already proves that this session exists.
             * Check the session directory for metadata by name before asking
             * the JSON reader to recover it. Incomplete sessions are common
             * after a power loss; avoiding negative stat/fopen probes for them
             * removes a slow FAT lookup series while retaining .bak recovery
             * whenever either metadata file is actually present.
             */
            seismic_recorder_fs_metadata_state_t metadata_state =
                SEISMIC_RECORDER_FS_METADATA_NONE;
            const esp_err_t metadata_err =
                seismic_recorder_fs_session_metadata_state(
                    sessions_path,
                    entry->d_name,
                    session_path,
                    SEISMIC_RECORDER_PATH_SIZE,
                    &metadata_state
                );
            if (metadata_err != ESP_OK) {
                err = metadata_err;
                break;
            }

            if (
                metadata_state == SEISMIC_RECORDER_FS_METADATA_NONE ||
                (
                    metadata_state == SEISMIC_RECORDER_FS_METADATA_PRIMARY
                        ? seismic_recorder_json_read_summary_known_present(
                            object_id,
                            entry->d_name,
                            &summary
                        )
                        : seismic_recorder_json_read_summary(
                            object_id,
                            entry->d_name,
                            &summary
                        )
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
    free(session_path);
    free(sessions_path);

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

    object_store_profile_t *profile = malloc(sizeof(*profile));
    if (profile == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = object_store_get_profile(object_id, profile);
    free(profile);
    if (err != ESP_OK) {
        return err;
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
    if (err != ESP_OK) {
        free(session_path);
        return err;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(session_path);
        return err;
    }

    err = storage_manager_remove_tree(session_path);

    storage_manager_operation_end();
    free(session_path);
    return err;
}
