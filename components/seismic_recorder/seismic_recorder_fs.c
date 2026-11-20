#include "seismic_recorder_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
#include "seismic_recorder_lod_store.h"
#include "seismic_recorder_storage_preflight.h"
#include "scan_artifact_path.h"
#include "storage_manager.h"

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#endif

static const char *TAG = "SEISMIC_RECORDER_FS";

typedef struct
{
    const char *file_name;
    unsigned mask;
} seismic_recorder_staged_artifact_t;

static const seismic_recorder_staged_artifact_t s_staged_artifacts[] = {
    {.file_name = "scan-v1.bin", .mask = 1U << 0U},
    {.file_name = "occupancy-v1.bin", .mask = 1U << 1U},
    {.file_name = "preview-v1.bin", .mask = 1U << 2U},
    {.file_name = "registration-v1.json", .mask = 1U << 3U},
    {.file_name = "tag-positions-v1.json", .mask = 1U << 4U},
};


typedef enum
{
    SEISMIC_RECORDER_FS_METADATA_NONE = 0,
    SEISMIC_RECORDER_FS_METADATA_PRIMARY,
    SEISMIC_RECORDER_FS_METADATA_BACKUP,

} seismic_recorder_fs_metadata_state_t;

/* Reuse the old DMA buffer for v1 records or all v2 blocks in one 256-frame window. */
typedef union
{
    seismic_frame_t v1_frames[SEISMIC_RECORDER_WRITE_STAGING_FRAMES];
    uint8_t v2_window[
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES *
        (SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
         SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES)
    ];
} seismic_recorder_dma_staging_t;

DMA_ATTR static seismic_recorder_dma_staging_t s_dma_staging;
static seismic_recorder_sample_format_t s_active_sample_format = {
    .version = SEISMIC_RECORDER_BINARY_VERSION_V1,
};
static uint64_t s_last_written_sample_index = 0U;
static bool s_have_written_sample_index = false;


static void seismic_recorder_fs_reset_sample_format(void)
{
    memset(&s_active_sample_format, 0, sizeof(s_active_sample_format));
    s_active_sample_format.version = SEISMIC_RECORDER_BINARY_VERSION_V1;
    s_last_written_sample_index = 0U;
    s_have_written_sample_index = false;
}

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
        err = seismic_recorder_fs_errno_to_error(errno);
        free(sessions_path);
        return err;
    }

    unsigned highest = 0U;
    struct dirent *entry = NULL;
    while (true) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                err = seismic_recorder_fs_errno_to_error(errno);
            }
            break;
        }

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

    const int close_result = closedir(directory);
    if (err == ESP_OK && close_result != 0) {
        err = seismic_recorder_fs_errno_to_error(errno);
    }

    free(sessions_path);

    if (err != ESP_OK) {
        return err;
    }

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


/*
 * One 3D scan serves every recording made with the same sensor placement. FAT has no links and copying an 8 MB scan to the
 * SD card takes minutes, so the scan, its occupancy (with the registered tags), preview and registration are stored once,
 * under <object>/scans/<SCnnnnnn>/, and a session carries a one-line reference (scan-ref) to it; see scan_artifact_path.h.
 *
 * At START the artifacts waiting in staging (a fresh upload) become a new shared scan and the new session refers to it. With
 * nothing in staging the new session refers to the scan of the newest earlier session that has one; a session recorded
 * before this scheme, which holds its own copy, hands that copy to the shared folder first (a rename, instant), so every
 * recording of a placement keeps its voxels. A scan made for another placement is not applied silently: consumers compare
 * its geometry_computed_at_ms with the session's frozen geometry.
 */
#define SEISMIC_RECORDER_FS_SCAN_LOOKBACK 64U

static bool seismic_recorder_fs_artifact_present(const char *dir, const char *name)
{
    char path[SEISMIC_RECORDER_PATH_SIZE];
    struct stat info;
    return snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path) && stat(path, &info) == 0 &&
        (info.st_mode & S_IFMT) == S_IFREG;
}

static unsigned seismic_recorder_fs_artifacts_present_mask(const char *dir)
{
    unsigned mask = 0U;
    for (size_t i = 0U; i < sizeof(s_staged_artifacts) / sizeof(s_staged_artifacts[0]); ++i) {
        if (seismic_recorder_fs_artifact_present(dir, s_staged_artifacts[i].file_name)) mask |= s_staged_artifacts[i].mask;
    }
    return mask;
}

/* The next free SCnnnnnn of the object: one above the highest in scans/. */
static bool seismic_recorder_fs_next_scan_id(const char *object_id, char id[SCAN_ARTIFACT_ID_SIZE])
{
    char dir_path[SEISMIC_RECORDER_PATH_SIZE];
    if (snprintf(dir_path, sizeof(dir_path), "%s/%s/scans", STORAGE_MANAGER_OBJECTS_PATH, object_id) >= (int)sizeof(dir_path)) {
        return false;
    }
    unsigned highest = 0U;
    DIR *dir = opendir(dir_path);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!scan_artifact_id_valid(entry->d_name)) continue;
            const unsigned number = (unsigned)strtoul(entry->d_name + 2, NULL, 10);
            if (number > highest) highest = number;
        }
        closedir(dir);
    }
    return highest < 999999U && snprintf(id, SCAN_ARTIFACT_ID_SIZE, "SC%06u", highest + 1U) == (int)SCAN_ARTIFACT_ID_SIZE - 1;
}

static bool seismic_recorder_fs_write_scan_ref(const char *session_dir, const char *scan_id)
{
    char path[SEISMIC_RECORDER_PATH_SIZE];
    if (snprintf(path, sizeof(path), "%s/%s", session_dir, SCAN_ARTIFACT_REF_FILE) >= (int)sizeof(path)) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) return false;
    const bool ok = fwrite(scan_id, 1U, SCAN_ARTIFACT_ID_SIZE - 1U, file) == SCAN_ARTIFACT_ID_SIZE - 1U;
    return fclose(file) == 0 && ok;
}

/* Moves every artifact in `from_dir` into a new shared scan; returns the artifacts moved (0 and nothing created if none). */
static unsigned seismic_recorder_fs_promote_scan(
    const char *object_id, const char *from_dir, char scan_id[SCAN_ARTIFACT_ID_SIZE])
{
    if (seismic_recorder_fs_artifacts_present_mask(from_dir) == 0U) return 0U;
    char scans_dir[SEISMIC_RECORDER_PATH_SIZE], target[SEISMIC_RECORDER_PATH_SIZE];
    if (!seismic_recorder_fs_next_scan_id(object_id, scan_id) ||
        snprintf(scans_dir, sizeof(scans_dir), "%s/%s/scans", STORAGE_MANAGER_OBJECTS_PATH, object_id) >= (int)sizeof(scans_dir) ||
        scan_artifact_shared_path(STORAGE_MANAGER_OBJECTS_PATH, object_id, scan_id, NULL, target, sizeof(target)) != 0 ||
        storage_manager_create_directory(scans_dir) != ESP_OK || storage_manager_create_directory(target) != ESP_OK) {
        ESP_LOGW(TAG, "Could not create a shared scan folder; the scan stays where it is");
        return 0U;
    }
    unsigned moved = 0U;
    for (size_t i = 0U; i < sizeof(s_staged_artifacts) / sizeof(s_staged_artifacts[0]); ++i) {
        const seismic_recorder_staged_artifact_t *artifact = &s_staged_artifacts[i];
        char from[SEISMIC_RECORDER_PATH_SIZE], to[SEISMIC_RECORDER_PATH_SIZE];
        if (!seismic_recorder_fs_artifact_present(from_dir, artifact->file_name)) continue;
        if (snprintf(from, sizeof(from), "%s/%s", from_dir, artifact->file_name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", target, artifact->file_name) >= (int)sizeof(to)) {
            continue;
        }
        if (rename(from, to) == 0) {
            moved |= artifact->mask;
        } else {
            ESP_LOGW(TAG, "Could not move %s into the shared scan %s: errno=%d", artifact->file_name, scan_id, errno);
        }
    }
    return moved;
}

/* Gives the new session its scan. Returns the artifacts taken out of staging (needed to put them back if START fails). */
static unsigned seismic_recorder_fs_attach_staged_artifacts(
    const char *object_id,
    char *session_path,
    size_t session_path_size
)
{
    (void)session_path_size;
    const char *session_id = strrchr(session_path, '/');
    session_id = session_id != NULL ? session_id + 1 : session_path;
    char staging_dir[SEISMIC_RECORDER_PATH_SIZE];
    if (snprintf(staging_dir, sizeof(staging_dir), "%s/%s/staging", STORAGE_MANAGER_OBJECTS_PATH, object_id) >=
        (int)sizeof(staging_dir)) {
        return 0U;
    }

    char scan_id[SCAN_ARTIFACT_ID_SIZE];
    /* A fresh upload becomes the scan of this placement. */
    const unsigned staged = seismic_recorder_fs_promote_scan(object_id, staging_dir, scan_id);
    if (staged != 0U) {
        if (!seismic_recorder_fs_write_scan_ref(session_path, scan_id)) {
            ESP_LOGW(TAG, "Could not record which scan %s uses; recording continues", session_id);
        }
        return staged;
    }

    /* Otherwise the same placement as the newest earlier recording that has a scan. */
    unsigned number = 0U;
    if (sscanf(session_id, "S%u", &number) != 1 || number <= 1U) return 0U;
    for (unsigned back = 1U; back <= SEISMIC_RECORDER_FS_SCAN_LOOKBACK && back < number; ++back) {
        char previous_dir[SEISMIC_RECORDER_PATH_SIZE];
        char previous_id[16];
        if (snprintf(previous_id, sizeof(previous_id), "S%06u", number - back) >= (int)sizeof(previous_id) ||
            scan_artifact_session_path(STORAGE_MANAGER_OBJECTS_PATH, object_id, previous_id, NULL, previous_dir,
                                       sizeof(previous_dir)) != 0) {
            break;
        }
        char shared_id[SCAN_ARTIFACT_ID_SIZE];
        if (scan_artifact_read_ref(STORAGE_MANAGER_OBJECTS_PATH, object_id, previous_id, shared_id)) {
            (void)seismic_recorder_fs_write_scan_ref(session_path, shared_id);
            return 0U;
        }
        if (seismic_recorder_fs_artifacts_present_mask(previous_dir) != 0U) {
            /* Recorded before the shared scans: hand its own copy over and let both sessions refer to it. */
            if (seismic_recorder_fs_promote_scan(object_id, previous_dir, shared_id) != 0U) {
                (void)seismic_recorder_fs_write_scan_ref(previous_dir, shared_id);
                (void)seismic_recorder_fs_write_scan_ref(session_path, shared_id);
            }
            return 0U;
        }
    }
    return 0U;
}

/* START failed after the scan was taken out of staging: put it back, as it was. */
static bool seismic_recorder_fs_restore_staged_artifacts(
    const char *object_id,
    const char *session_id,
    char *session_path,
    size_t session_path_size,
    unsigned moved_mask
)
{
    if (seismic_recorder_fs_build_session_path(object_id, session_id, session_path, session_path_size) != ESP_OK) {
        return false;
    }
    if (moved_mask == 0U) return true;

    char scan_id[SCAN_ARTIFACT_ID_SIZE];
    if (!scan_artifact_read_ref(STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, scan_id)) return false;
    char staging_dir[SEISMIC_RECORDER_PATH_SIZE], shared_dir[SEISMIC_RECORDER_PATH_SIZE];
    if (snprintf(staging_dir, sizeof(staging_dir), "%s/%s/staging", STORAGE_MANAGER_OBJECTS_PATH, object_id) >=
            (int)sizeof(staging_dir) ||
        scan_artifact_shared_path(STORAGE_MANAGER_OBJECTS_PATH, object_id, scan_id, NULL, shared_dir, sizeof(shared_dir)) != 0) {
        return false;
    }
    bool restored = true;
    for (size_t i = 0U; i < sizeof(s_staged_artifacts) / sizeof(s_staged_artifacts[0]); ++i) {
        const seismic_recorder_staged_artifact_t *artifact = &s_staged_artifacts[i];
        if ((moved_mask & artifact->mask) == 0U) continue;
        char from[SEISMIC_RECORDER_PATH_SIZE], to[SEISMIC_RECORDER_PATH_SIZE];
        if (snprintf(from, sizeof(from), "%s/%s", shared_dir, artifact->file_name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", staging_dir, artifact->file_name) >= (int)sizeof(to) ||
            rename(from, to) != 0) {
            restored = false;
            ESP_LOGW(TAG, "Could not restore staged %s after start failure: errno=%d", artifact->file_name, errno);
        }
    }
    if (restored) {
        char ref_path[SEISMIC_RECORDER_PATH_SIZE];
        if (snprintf(ref_path, sizeof(ref_path), "%s/%s", session_path, SCAN_ARTIFACT_REF_FILE) < (int)sizeof(ref_path)) {
            (void)remove(ref_path);
        }
        (void)storage_manager_remove_tree(shared_dir); /* empty again: every artifact went back to staging */
    }
    return restored;
}


esp_err_t seismic_recorder_fs_create_session(
    const char *object_id,
    const seismic_acquisition_snapshot_t *acquisition,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
)
{
    if (
        object_id == NULL ||
        acquisition == NULL ||
        session_id == NULL ||
        samples_file == NULL ||
        format_version == NULL ||
        acquisition->sample_rate_hz == 0U ||
        channel_count == 0U
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_file = NULL;
    *format_version = 0U;
    session_id[0] = '\0';

    seismic_recorder_sample_format_t sample_format;
    esp_err_t err = seismic_recorder_samples_format_from_acquisition(
        acquisition,
        &sample_format
    );
    if (err != ESP_OK) {
        return err;
    }

    const int64_t operation_started_us =
        esp_timer_get_time();

    char *path = malloc(SEISMIC_RECORDER_PATH_SIZE);
    if (path == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = storage_manager_operation_begin();
    if (err != ESP_OK) {
        free(path);
        return err;
    }

    bool session_directory_created = false;
    unsigned staged_artifact_mask = 0U;

    /*
     * The free-space query runs inside the same outer recursive storage
     * operation as the following session creation. Other storage workers
     * therefore cannot consume the admitted runway between the check and the
     * Sxxxxxx/samples.bin commit path.
     */
    err = seismic_recorder_storage_preflight(
        acquisition->sample_rate_hz
    );

    if (err == ESP_OK) {
        err = seismic_recorder_fs_next_session_id(
            object_id,
            session_id
        );
    }

    if (err == ESP_OK) {
        err = seismic_recorder_fs_build_session_path(
            object_id,
            session_id,
            path,
            SEISMIC_RECORDER_PATH_SIZE
        );
    }

    if (err == ESP_OK) {
        err = storage_manager_create_directory_exclusive(path);
        session_directory_created = err == ESP_OK;
    }

    if (err == ESP_OK) {
        /* START already holds storage_manager's operation guard; keep each rename atomic. */
        staged_artifact_mask = seismic_recorder_fs_attach_staged_artifacts(
            object_id,
            path,
            SEISMIC_RECORDER_PATH_SIZE
        );
    }

    /* LOD is an acceleration index, never part of the authoritative session
     * commit. Failure to create it must not prevent samples.bin recording. */
    if (err == ESP_OK && channel_count == SEISMIC_CHANNEL_COUNT) {
        const esp_err_t lod_err = seismic_recorder_lod_store_begin(
            path,
            acquisition->sample_rate_hz
        );
        if (lod_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "LOD64 sidecar unavailable; raw recording continues: %s",
                esp_err_to_name(lod_err)
            );
        }
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
            .version = sample_format.version,
            .channel_count = channel_count,
            .sample_rate_hz = acquisition->sample_rate_hz,
            .frame_size_bytes = sample_format.version == SEISMIC_RECORDER_BINARY_VERSION_V2
                ? SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
                : sizeof(seismic_frame_t),
            .reserved = 0U,
        };

        if (fwrite(&header, sizeof(header), 1U, file) != 1U) {
            err = ESP_FAIL;
        }
        if (err == ESP_OK && sample_format.version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
            seismic_recorder_binary_v2_metadata_t metadata;
            seismic_recorder_samples_v2_metadata_from_format(
                &sample_format,
                &metadata
            );
            if (fwrite(&metadata, sizeof(metadata), 1U, file) != 1U) {
                err = ESP_FAIL;
            }
        }
    }

    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        seismic_recorder_lod_store_abort();
        if (file != NULL) {
            fclose(file);
        }
        if (session_directory_created) {
            const bool artifacts_restored = seismic_recorder_fs_restore_staged_artifacts(
                object_id,
                session_id,
                path,
                SEISMIC_RECORDER_PATH_SIZE,
                staged_artifact_mask
            );
            if (artifacts_restored) {
                const esp_err_t cleanup_err =
                    storage_manager_remove_tree(path);
                if (
                    cleanup_err != ESP_OK &&
                    cleanup_err != ESP_ERR_NOT_FOUND
                ) {
                    ESP_LOGW(
                        TAG,
                        "Failed to clean incomplete session %s: %s",
                        session_id,
                        esp_err_to_name(cleanup_err)
                    );
                }
            }
            else {
                ESP_LOGW(
                    TAG,
                    "Start failed and staged artifact rollback was incomplete; preserving session %s",
                    session_id
                );
            }
        }
    }
    else {
        *samples_file = file;
        *format_version = sample_format.version;
        s_active_sample_format = sample_format;
        s_last_written_sample_index = 0U;
        s_have_written_sample_index = false;
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
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t *frames_written
)
{
    if (
        samples_file == NULL ||
        frames == NULL ||
        sample_indices == NULL ||
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

        /* Release the LVGL/SPI2 guard between bounded writes. A slow SD
         * card must not hold the TFT mutex for the whole recorder batch. */
        const int64_t window_started_us = esp_timer_get_time();
        err = storage_manager_operation_begin();
        if (err != ESP_OK) {
            break;
        }

        err = ESP_OK;
        size_t chunk_written = 0U;
        if (s_active_sample_format.version == SEISMIC_RECORDER_BINARY_VERSION_V1) {
            memcpy(
                s_dma_staging.v1_frames,
                frames + written,
                chunk * sizeof(seismic_frame_t)
            );
            errno = 0;
            chunk_written = fwrite(
                s_dma_staging.v1_frames,
                sizeof(seismic_frame_t),
                chunk,
                samples_file
            );
            if (chunk_written != chunk) {
                /* The one place a short write is visible as such: say why (a full volume, a closed file, no DMA bounce buffer). */
                ESP_LOGE(
                    TAG,
                    "RECORDER_WRITE_FAILED frames=%u written=%u errno=%d ferror=%d",
                    (unsigned)chunk,
                    (unsigned)chunk_written,
                    errno,
                    ferror(samples_file)
                );
            }
            written += chunk_written;
        }
        else if (s_active_sample_format.version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
            size_t encoded_frames = 0U;
            size_t encoded_bytes = 0U;
            while (encoded_frames < chunk) {
                const size_t run_start = encoded_frames;
                size_t run_count = 1U;
                while (run_start + run_count < chunk) {
                    const uint64_t previous =
                        sample_indices[written + run_start + run_count - 1U];
                    if (
                        previous == UINT64_MAX ||
                        sample_indices[written + run_start + run_count] != previous + 1U
                    ) {
                        break;
                    }
                    ++run_count;
                }

                bool gap_before = false;
                if (run_start > 0U || s_have_written_sample_index) {
                    const uint64_t previous = run_start > 0U
                        ? sample_indices[written + run_start - 1U]
                        : s_last_written_sample_index;
                    const uint64_t first = sample_indices[written + run_start];
                    if (
                        previous == UINT64_MAX ||
                        first <= previous
                    ) {
                        err = ESP_ERR_INVALID_RESPONSE;
                        break;
                    }
                    gap_before = first > previous + 1U;
                }

                size_t block_bytes = 0U;
                err = seismic_recorder_samples_encode_v2_block(
                    &s_active_sample_format,
                    frames + written + run_start,
                    sample_indices + written + run_start,
                    run_count,
                    gap_before,
                    s_dma_staging.v2_window + encoded_bytes,
                    sizeof(s_dma_staging.v2_window) - encoded_bytes,
                    &block_bytes
                );
                if (err != ESP_OK) {
                    break;
                }
                encoded_bytes += block_bytes;
                encoded_frames += run_count;
            }
            if (err == ESP_OK) {
                errno = 0;
                if (fwrite(s_dma_staging.v2_window, 1U, encoded_bytes, samples_file) != encoded_bytes) {
                    /* The one place a short write is visible as such: say why (a full volume, a closed file, no DMA bounce buffer). */
                    ESP_LOGE(
                        TAG,
                        "RECORDER_WRITE_FAILED bytes=%u errno=%d ferror=%d",
                        (unsigned)encoded_bytes,
                        errno,
                        ferror(samples_file)
                    );
                    err = ESP_FAIL;
                }
                else {
                    chunk_written = encoded_frames;
                    written += chunk_written;
                    s_last_written_sample_index = sample_indices[written - 1U];
                    s_have_written_sample_index = true;
                }
            }
        }
        else {
            err = ESP_ERR_INVALID_STATE;
        }
        storage_manager_operation_end();
        ++storage_windows;

        const int64_t storage_window_us =
            esp_timer_get_time() - window_started_us;
        if (storage_window_us > max_storage_window_us) {
            max_storage_window_us = storage_window_us;
        }

        /* Feed only frames that samples.bin actually accepted. The optional
         * LOD write owns a separate bounded storage window, so LVGL can run
         * between the raw write and index maintenance. Keep the raw-window
         * timing above independent from this optional index work. */
        if (chunk_written > 0U && seismic_recorder_lod_store_is_active()) {
            const size_t input_start = written - chunk_written;
            const seismic_frame_t *lod_frames = frames + input_start;
            esp_err_t lod_err = ESP_OK;
            if (s_active_sample_format.version == SEISMIC_RECORDER_BINARY_VERSION_V2) {
                size_t timestamp_run_start = 0U;
                uint64_t run_first_timestamp_us = 0U;
                for (size_t index = 0U; index < chunk_written; ++index) {
                    lod_err =
                        seismic_recorder_samples_quantize_frame(
                            &s_active_sample_format,
                            &lod_frames[index],
                            &s_dma_staging.v1_frames[index]
                        );
                    if (lod_err != ESP_OK) {
                        break;
                    }

                    const uint64_t current_index = sample_indices[input_start + index];
                    if (
                        index == 0U ||
                        sample_indices[input_start + index - 1U] == UINT64_MAX ||
                        current_index != sample_indices[input_start + index - 1U] + 1U
                    ) {
                        timestamp_run_start = index;
                        if (frames[input_start + index].timestamp_us < 0) {
                            lod_err = ESP_ERR_INVALID_RESPONSE;
                            break;
                        }
                        run_first_timestamp_us =
                            (uint64_t)frames[input_start + index].timestamp_us;
                    }
                    uint64_t timestamp_us = 0U;
                    if (
                        !seismic_recorder_samples_timestamp_at(
                            run_first_timestamp_us,
                            (uint64_t)(index - timestamp_run_start),
                            s_active_sample_format.sample_rate_hz,
                            &timestamp_us
                        ) ||
                        timestamp_us > (uint64_t)INT64_MAX
                    ) {
                        lod_err = ESP_ERR_INVALID_RESPONSE;
                        break;
                    }
                    s_dma_staging.v1_frames[index].timestamp_us =
                        (int64_t)timestamp_us;
                }
                lod_frames = s_dma_staging.v1_frames;
            }
            if (lod_err == ESP_OK) {
                lod_err = seismic_recorder_lod_store_append(
                    lod_frames,
                    chunk_written
                );
            }
            if (lod_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "LOD64 sidecar disabled; raw samples remain authoritative: %s",
                    esp_err_to_name(lod_err)
                );
            }
        }

        if (
            err != ESP_OK ||
            chunk_written != chunk ||
            ferror(samples_file) != 0
        ) {
            /* Every cause ends as ESP_FAIL for the caller; the log keeps the first one (encoder, short write, sticky error). */
            ESP_LOGE(
                TAG,
                "RECORDER_WRITE_ABORT cause=%s chunk_written=%u chunk=%u ferror=%d errno=%d",
                esp_err_to_name(err),
                (unsigned)chunk_written,
                (unsigned)chunk,
                ferror(samples_file),
                errno
            );
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

    if (seismic_recorder_lod_store_is_active()) {
        const esp_err_t lod_err =
            seismic_recorder_lod_store_flush(sync_to_media);
        if (lod_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "LOD64 flush failed; raw session continues: %s",
                esp_err_to_name(lod_err)
            );
        }
    }

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

    if (seismic_recorder_lod_store_is_active()) {
        const esp_err_t lod_err = seismic_recorder_lod_store_flush(true);
        if (lod_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "LOD64 sync failed; raw session remains authoritative: %s",
                esp_err_to_name(lod_err)
            );
        }
    }

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
        seismic_recorder_fs_reset_sample_format();
        seismic_recorder_lod_store_abort();
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
    seismic_recorder_fs_reset_sample_format();

    if (seismic_recorder_lod_store_is_active()) {
        const esp_err_t lod_err = seismic_recorder_lod_store_close();
        if (lod_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "LOD64 close failed; raw session close result is unchanged: %s",
                esp_err_to_name(lod_err)
            );
        }
    }

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

            const esp_err_t summary_err =
                metadata_state == SEISMIC_RECORDER_FS_METADATA_NONE
                    ? ESP_ERR_NOT_FOUND
                    : metadata_state == SEISMIC_RECORDER_FS_METADATA_PRIMARY
                        ? seismic_recorder_json_read_summary_known_present(
                            object_id,
                            entry->d_name,
                            &summary
                        )
                        : seismic_recorder_json_read_summary(
                            object_id,
                            entry->d_name,
                            &summary
                        );
            if (
                summary_err != ESP_OK &&
                !seismic_recorder_summary_error_is_unreadable_metadata(summary_err)
            ) {
                err = summary_err;
                break;
            }

            if (summary_err != ESP_OK) {
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
        const int close_result = closedir(directory);
        if (err == ESP_OK && close_result != 0) {
            err = seismic_recorder_fs_errno_to_error(errno);
        }
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
