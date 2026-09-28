/*
 * This test includes the production filesystem translation unit so the host
 * binary exercises its real header/session/path code. Storage and object
 * boundaries are faked below; no user SD path is touched.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir(path) _rmdir(path)
#else
#include <unistd.h>
#endif

#define STORAGE_MANAGER_MOUNT_POINT "seismic_recorder_contract_tmp"

#include "seismic_recorder.h"
#include "seismic_buffer.h"
#include "object_store.h"
#include "storage_manager.h"
#include "seismic_recorder_cache.h"

/* The embedded libc declares these through its POSIX compatibility headers. */
int fileno(FILE *stream);
size_t strnlen(const char *string, size_t max_length);

static unsigned storage_begin_calls;
static unsigned storage_end_calls;
static esp_err_t storage_begin_error;
static bool fail_create_directory;
static unsigned exclusive_directory_calls;
static unsigned remove_tree_calls;
static char removed_tree_path[256];
static esp_err_t fake_remove_tree_error;
static esp_err_t fake_summary_error = ESP_FAIL;
static bool fake_summary_override_active;
static seismic_recorder_session_summary_t fake_summary_override;
static size_t fake_summary_read_calls;
static size_t fake_known_present_summary_read_calls;
static size_t recorder_test_fwrite_calls;
static size_t injected_short_write_count = SIZE_MAX;
static bool injected_fflush_error;
static bool injected_fsync_error;
static bool injected_fclose_error;
static char injected_rename_source[256];
static int seismic_recorder_test_rename(const char *source, const char *destination);

typedef enum
{
    SEISMIC_RECORDER_DIRECTORY_FAULT_NONE = 0,
    SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR,
    SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR,
    SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR,

} seismic_recorder_directory_fault_t;

static seismic_recorder_directory_fault_t directory_fault;
static unsigned directory_fault_call;
static int directory_fault_errno;
static unsigned directory_opendir_calls;
static unsigned directory_readdir_calls;
static unsigned directory_closedir_calls;

static void reset_directory_fault(void)
{
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_NONE;
    directory_fault_call = 0U;
    directory_fault_errno = 0;
    directory_opendir_calls = 0U;
    directory_readdir_calls = 0U;
    directory_closedir_calls = 0U;
}

static DIR *seismic_recorder_test_opendir(const char *path)
{
    ++directory_opendir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR &&
        directory_opendir_calls == directory_fault_call
    ) {
        errno = directory_fault_errno;
        return NULL;
    }

    return opendir(path);
}

static struct dirent *seismic_recorder_test_readdir(DIR *directory)
{
    ++directory_readdir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR &&
        directory_readdir_calls == directory_fault_call
    ) {
        errno = directory_fault_errno;
        return NULL;
    }

    return readdir(directory);
}

static int seismic_recorder_test_closedir(DIR *directory)
{
    ++directory_closedir_calls;
    if (
        directory_fault == SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR &&
        directory_closedir_calls == directory_fault_call
    ) {
        const int result = closedir(directory);
        errno = directory_fault_errno;
        return result == 0 ? -1 : result;
    }

    return closedir(directory);
}

static int make_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static void make_layout(void)
{
    assert(make_directory(STORAGE_MANAGER_MOUNT_POINT) == 0);
    assert(make_directory(STORAGE_MANAGER_ROOT_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH) == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions") == 0);
}

static const char *const staged_artifact_names[] = {
    "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
};

static const char *const staged_artifact_contents[] = {
    "scan", "occupancy", "preview", "registration",
};

static bool path_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static void write_staged_artifacts(void)
{
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging") == 0);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char path[256];
        const int written = snprintf(
            path,
            sizeof(path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        assert(written > 0 && (size_t)written < sizeof(path));
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        const size_t length = strlen(staged_artifact_contents[i]);
        assert(fwrite(staged_artifact_contents[i], 1U, length, file) == length);
        assert(fclose(file) == 0);
    }
}

static void assert_file_contents(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    char actual[32] = {0};
    const size_t expected_length = strlen(expected);
    assert(expected_length < sizeof(actual));
    assert(fread(actual, 1U, sizeof(actual), file) == expected_length);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    assert(memcmp(actual, expected, expected_length) == 0);
}

static void remove_layout(void)
{
    char sessions_path[256];
    (void)snprintf(
        sessions_path,
        sizeof(sessions_path),
        "%s/OBJ_0001/sessions",
        STORAGE_MANAGER_OBJECTS_PATH
    );
    DIR *sessions = opendir(sessions_path);
    if (sessions != NULL) {
        struct dirent *entry = NULL;
        while ((entry = readdir(sessions)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char session_path[256];
            char samples_path[256];
            char marker_path[256];
            char metadata_path[256];
            char backup_path[256];
            static const char *const artifact_names[] = {
                "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
            };
            (void)snprintf(session_path, sizeof(session_path), "%s/%s", sessions_path, entry->d_name);
            (void)snprintf(samples_path, sizeof(samples_path), "%s/samples.bin", session_path);
            (void)snprintf(marker_path, sizeof(marker_path), "%s/marker", session_path);
            (void)snprintf(metadata_path, sizeof(metadata_path), "%s/session.json", session_path);
            (void)snprintf(backup_path, sizeof(backup_path), "%s/session.json.bak", session_path);
            (void)remove(samples_path);
            (void)remove(marker_path);
            (void)remove(metadata_path);
            (void)remove(backup_path);
            for (size_t artifact_index = 0U; artifact_index < sizeof(artifact_names) / sizeof(artifact_names[0]); ++artifact_index) {
                char artifact_path[256];
                (void)snprintf(artifact_path, sizeof(artifact_path), "%s/%s", session_path, artifact_names[artifact_index]);
                (void)remove(artifact_path);
            }
            (void)rmdir(session_path);
        }
        (void)closedir(sessions);
    }
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    static const char *const staged_names[] = {
        "scan-v1.bin", "occupancy-v1.bin", "preview-v1.bin", "registration-v1.json",
    };
    for (size_t i = 0U; i < sizeof(staged_names) / sizeof(staged_names[0]); ++i) {
        char staged_path[256];
        (void)snprintf(staged_path, sizeof(staged_path), STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s", staged_names[i]);
        (void)remove(staged_path);
    }
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}

/* Boundary fakes used by the production filesystem implementation. */
esp_err_t storage_manager_operation_begin(void)
{
    ++storage_begin_calls;
    return storage_begin_error;
}

void storage_manager_operation_end(void)
{
    ++storage_end_calls;
}

esp_err_t storage_manager_create_directory(const char *path)
{
    if (fail_create_directory) {
        return ESP_FAIL;
    }
    return make_directory(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_manager_create_directory_exclusive(const char *path)
{
    ++exclusive_directory_calls;
    if (fail_create_directory) {
        return ESP_FAIL;
    }
    errno = 0;
    if (mkdir(path, 0775) == 0) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++remove_tree_calls;
    (void)snprintf(removed_tree_path, sizeof(removed_tree_path), "%s", path);
    return fake_remove_tree_error;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    (void)object_id;
    (void)profile;
    return ESP_OK;
}

static esp_err_t fake_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary,
    bool known_present
)
{
    (void)object_id;
    if (known_present) {
        ++fake_known_present_summary_read_calls;
    }
    else {
        ++fake_summary_read_calls;
    }
    if (fake_summary_error != ESP_OK || summary == NULL) {
        return fake_summary_error;
    }
    if (fake_summary_override_active) {
        *summary = fake_summary_override;
        snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
        return ESP_OK;
    }
    memset(summary, 0, sizeof(*summary));
    snprintf(summary->session_id, sizeof(summary->session_id), "%s", session_id);
    snprintf(summary->source_name, sizeof(summary->source_name), "AD7606");
    summary->complete = true;
    summary->sample_rate_hz = 500U;
    summary->channel_count = SEISMIC_CHANNEL_COUNT;
    summary->samples_written = 5U;
    return ESP_OK;
}

/* Not reached by these tests, but required when this TU includes fs.c. */
esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_read_summary(
        object_id,
        session_id,
        summary,
        false
    );
}

esp_err_t seismic_recorder_json_read_summary_known_present(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    return fake_read_summary(
        object_id,
        session_id,
        summary,
        true
    );
}

static size_t recorder_test_fwrite(
    const void *buffer,
    size_t element_size,
    size_t element_count,
    FILE *stream
)
{
    ++recorder_test_fwrite_calls;
    const size_t count = injected_short_write_count == SIZE_MAX
        ? element_count
        : (injected_short_write_count < element_count ? injected_short_write_count : element_count);
    return fwrite(buffer, element_size, count, stream);
}

static int recorder_test_fflush(FILE *stream)
{
    if (injected_fflush_error) return EOF;
    return fflush(stream);
}

static int recorder_test_fsync(int descriptor)
{
    (void)descriptor;
    return injected_fsync_error ? -1 : 0;
}

static int recorder_test_fclose(FILE *stream)
{
    const int result = fclose(stream);
    return injected_fclose_error && result == 0 ? EOF : result;
}

#define fwrite recorder_test_fwrite
#define fflush recorder_test_fflush
#ifdef _WIN32
#define _commit recorder_test_fsync
#else
#define fsync recorder_test_fsync
#endif
#define fclose recorder_test_fclose
#define rename seismic_recorder_test_rename
#define opendir seismic_recorder_test_opendir
#define readdir seismic_recorder_test_readdir
#define closedir seismic_recorder_test_closedir
#include "../seismic_recorder_samples.c"
#include "../seismic_recorder_fs.c"
#include "../seismic_recorder_reader.c"
#undef fwrite
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
#undef fclose
#undef rename
#undef opendir
#undef readdir
#undef closedir

bool seismic_recorder_cache_contains_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    uint64_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frame_count;
    return false;
}

esp_err_t seismic_recorder_cache_copy_session_range(
    const char *object_id,
    const char *session_id,
    uint64_t first_session_frame,
    seismic_frame_t *frames,
    size_t frame_count
)
{
    (void)object_id;
    (void)session_id;
    (void)first_session_frame;
    (void)frames;
    (void)frame_count;
    return ESP_ERR_NOT_FOUND;
}


static esp_err_t test_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
);
static esp_err_t test_fs_create_ad7606_session(
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
);
static esp_err_t test_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
);
static esp_err_t test_fs_write_frames_with_source_indices(
    FILE *samples_file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t *frames_written
);


static void set_fake_range_summary(
    const char *session_id,
    uint16_t format_version,
    uint64_t samples_written
)
{
    memset(&fake_summary_override, 0, sizeof(fake_summary_override));
    snprintf(
        fake_summary_override.session_id,
        sizeof(fake_summary_override.session_id),
        "%s",
        session_id
    );
    fake_summary_override.complete = true;
    fake_summary_override.readable = true;
    fake_summary_override.sample_rate_hz = 500U;
    fake_summary_override.channel_count = SEISMIC_CHANNEL_COUNT;
    fake_summary_override.format_version = format_version;
    fake_summary_override.samples_written = samples_written;
    fake_summary_override_active = true;
    fake_summary_error = ESP_OK;
}


static void test_session_range_v1_preserves_timestamps_and_bounds(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    assert(test_fs_create_session(
        "OBJ_0001", 500U, SEISMIC_CHANNEL_COUNT, session, &samples
    ) == ESP_OK);
    seismic_frame_t written[10] = {0};
    for (size_t i = 0U; i < 10U; ++i) {
        written[i].timestamp_us = 123456LL + (int64_t)i * 1987LL;
        written[i].channel_uv[0] = (float)i;
    }
    size_t frames_written = 0U;
    assert(test_fs_write_frames(samples, written, 10U, &frames_written) == ESP_OK);
    assert(frames_written == 10U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    set_fake_range_summary(session, SEISMIC_RECORDER_BINARY_VERSION_V1, 10U);

    seismic_frame_t frames[5] = {0};
    uint64_t indices[5] = {0};
    bool gaps[5] = {false};
    size_t count = 0U;
    seismic_recorder_session_summary_t summary;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 3U, 4U, frames, indices, gaps, &count, &summary
    ) == ESP_OK);
    assert(count == 4U && summary.format_version == SEISMIC_RECORDER_BINARY_VERSION_V1);
    for (size_t i = 0U; i < count; ++i) {
        assert(frames[i].timestamp_us == written[i + 3U].timestamp_us);
        assert(indices[i] == (uint64_t)i + 3U);
        assert(!gaps[i]);
    }

    count = 99U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 11U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_ERR_INVALID_ARG);
    assert(count == 0U);
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 10U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 0U);

    fake_summary_override_active = false;
    fake_summary_error = ESP_FAIL;
    remove_layout();
}


static void test_session_range_v2_seeks_blocks_and_preserves_gaps(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    const size_t total = 262U;
    seismic_frame_t written[262] = {0};
    uint64_t source_indices[262] = {0};
    for (size_t i = 0U; i < total; ++i) {
        source_indices[i] = i < 258U ? (uint64_t)i : (uint64_t)i + 3U;
        written[i].timestamp_us = (int64_t)source_indices[i] * 2000LL;
        written[i].channel_uv[0] = (float)(int)(i % 100U);
    }
    size_t frames_written = 0U;
    assert(test_fs_write_frames_with_source_indices(
        samples, written, source_indices, total, &frames_written
    ) == ESP_OK);
    assert(frames_written == total);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    set_fake_range_summary(session, version, total);

    seismic_frame_t frames[8] = {0};
    uint64_t indices[8] = {0};
    bool gaps[8] = {false};
    size_t count = 0U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 254U, 8U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 8U);
    const uint64_t expected_indices[8] = {254U, 255U, 256U, 257U, 261U, 262U, 263U, 264U};
    for (size_t i = 0U; i < count; ++i) {
        assert(indices[i] == expected_indices[i]);
        assert(frames[i].timestamp_us == (int64_t)indices[i] * 2000LL);
        assert(gaps[i] == (i == 4U));
    }

    /* An early range must not inspect the later block's header or payload. */
    char path[SEISMIC_RECORDER_PATH_SIZE];
    assert(seismic_recorder_fs_build_session_path(
        "OBJ_0001", session, path, sizeof(path)
    ) == ESP_OK);
    const size_t path_length = strlen(path);
    assert(path_length + sizeof("/samples.bin") <= sizeof(path));
    memcpy(path + path_length, "/samples.bin", sizeof("/samples.bin"));
    FILE *corrupt = fopen(path, "r+b");
    assert(corrupt != NULL);
    const long final_block_header_offset = (long)(
        sizeof(seismic_recorder_binary_header_t) +
        sizeof(seismic_recorder_binary_v2_metadata_t) +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        SEISMIC_RECORDER_WRITE_STAGING_FRAMES * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES +
        SEISMIC_RECORDER_V2_BLOCK_HEADER_BYTES +
        2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES
    );
    assert(fseek(corrupt, final_block_header_offset + 16L, SEEK_SET) == 0);
    const uint16_t invalid_frame_count = 0U;
    assert(fwrite(&invalid_frame_count, sizeof(invalid_frame_count), 1U, corrupt) == 1U);
    assert(fclose(corrupt) == 0);

    count = 0U;
    assert(seismic_recorder_read_session_range(
        "OBJ_0001", session, 0U, 1U, frames, indices, gaps, &count, NULL
    ) == ESP_OK);
    assert(count == 1U && indices[0] == 0U && !gaps[0]);

    fake_summary_override_active = false;
    fake_summary_error = ESP_FAIL;
    remove_layout();
}

static esp_err_t test_fs_create_session(
    const char *object_id,
    uint32_t sample_rate_hz,
    uint8_t channel_count,
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file
)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "TEST",
        .sample_rate_hz = sample_rate_hz,
    };
    uint16_t version = 0U;
    return seismic_recorder_fs_create_session(
        object_id,
        &acquisition,
        channel_count,
        session_id,
        samples_file,
        &version
    );
}


static esp_err_t test_fs_create_ad7606_session(
    char session_id[SEISMIC_RECORDER_SESSION_ID_SIZE],
    FILE **samples_file,
    uint16_t *format_version
)
{
    const seismic_acquisition_snapshot_t acquisition = {
        .source_name = "AD7606",
        .sample_rate_hz = 500U,
        .bus_clock_hz = 10000000U,
        .channel_count = SEISMIC_CHANNEL_COUNT,
        .oversampling_ratio = 4U,
        .input_range_v = 5.0f,
    };
    return seismic_recorder_fs_create_session(
        "OBJ_0001",
        &acquisition,
        SEISMIC_CHANNEL_COUNT,
        session_id,
        samples_file,
        format_version
    );
}


static esp_err_t test_fs_write_frames_with_source_indices(
    FILE *samples_file,
    const seismic_frame_t *frames,
    const uint64_t *sample_indices,
    size_t frame_count,
    size_t *frames_written
)
{
    return seismic_recorder_fs_write_frames(
        samples_file,
        frames,
        sample_indices,
        frame_count,
        frames_written
    );
}


static esp_err_t test_fs_write_frames(
    FILE *samples_file,
    const seismic_frame_t *frames,
    size_t frame_count,
    size_t *frames_written
)
{
    uint64_t sample_indices[4096];
    if (frame_count > sizeof(sample_indices) / sizeof(sample_indices[0])) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index = 0U; index < frame_count; ++index) {
        sample_indices[index] = (uint64_t)index + 1U;
    }
    return seismic_recorder_fs_write_frames(
        samples_file,
        frames,
        sample_indices,
        frame_count,
        frames_written
    );
}


#define seismic_recorder_fs_create_session test_fs_create_session
#define seismic_recorder_fs_write_frames test_fs_write_frames

static int seismic_recorder_test_rename(const char *source, const char *destination)
{
    if (injected_rename_source[0] != '\0' && strcmp(source, injected_rename_source) == 0) {
        injected_rename_source[0] = '\0';
        errno = EIO;
        return -1;
    }
    return rename(source, destination);
}

static void test_invalid_arguments(void)
{
    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session(NULL, 50, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001/..", 50, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001", 0, 4, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 0, session, &file) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_write_frames(NULL, NULL, 1, NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_flush(NULL, false) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_sync(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_close(NULL) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_fs_delete_session("OBJ_0001", "S000001/..") == ESP_ERR_INVALID_ARG);
}

static void test_session_id_parser_is_canonical(void)
{
    char path[256];
    assert(
        seismic_recorder_fs_build_session_path(
            "OBJ_0001",
            "S000001",
            path,
            sizeof(path)
        ) == ESP_OK
    );
    assert(
        seismic_recorder_fs_build_session_path(
            "OBJ_0001",
            "S999999",
            path,
            sizeof(path)
        ) == ESP_OK
    );

    const char *invalid_ids[] = {
        "S1",
        "S00000a",
        "S000001x",
        "S000000",
        "S1000000",
    };
    for (size_t index = 0U; index < sizeof(invalid_ids) / sizeof(invalid_ids[0]); ++index) {
        assert(
            seismic_recorder_fs_build_session_path(
                "OBJ_0001",
                invalid_ids[index],
                path,
                sizeof(path)
            ) == ESP_ERR_INVALID_ARG
        );
    }
}

static void test_session_header_and_frames(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0;
    storage_end_calls = 0;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 1U);

    seismic_frame_t frames[2] = {
        {.timestamp_us = 100, .channel_uv = {1.0f, 2.0f, 3.0f, 4.0f}},
        {.timestamp_us = 200, .channel_uv = {-1.0f, -2.0f, -3.0f, -4.0f}},
    };
    size_t frames_written = 0;
    assert(seismic_recorder_fs_write_frames(file, frames, 2, &frames_written) == ESP_OK);
    assert(frames_written == 2U);
    assert(storage_begin_calls == 2U);
    assert(storage_end_calls == 2U);
    assert(seismic_recorder_fs_flush(file, false) == ESP_OK);
    assert(seismic_recorder_fs_sync(file) == ESP_OK);
    assert(storage_begin_calls == 4U);
    assert(storage_end_calls == 4U);
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    assert(storage_begin_calls == 5U);
    assert(storage_end_calls == 5U);

    file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin", "rb");
    assert(file != NULL);
    seismic_recorder_binary_header_t header;
    assert(fread(&header, sizeof(header), 1, file) == 1U);
    assert(memcmp(header.magic, "GEOSEIS1", 8) == 0);
    assert(header.version == 1U);
    assert(header.channel_count == 4U);
    assert(header.sample_rate_hz == 500U);
    assert(sizeof(seismic_frame_t) == 24U);
    assert(sizeof(seismic_recorder_binary_header_t) == 24U);
    assert(header.frame_size_bytes == 24U);
    seismic_frame_t read_frames[2] = {0};
    assert(fread(read_frames, sizeof(read_frames[0]), 2, file) == 2U);
    assert(read_frames[0].timestamp_us == 100);
    assert(read_frames[1].channel_uv[3] == -4.0f);
    assert(fclose(file) == 0);
    remove_layout();
}


static int16_t test_read_i16_le(const uint8_t *bytes)
{
    const uint16_t raw = (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
    int16_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}


static esp_err_t test_file_read_at(
    void *context,
    uint64_t offset_bytes,
    void *buffer,
    size_t size_bytes
)
{
    FILE *file = (FILE *)context;
    return
        offset_bytes <= (uint64_t)LONG_MAX &&
        fseek(file, (long)offset_bytes, SEEK_SET) == 0 &&
        fread(buffer, 1U, size_bytes, file) == size_bytes
            ? ESP_OK
            : ESP_ERR_INVALID_RESPONSE;
}


#if defined(SEISMIC_RECORDER_GAP_PIPELINE_TEST)
static void publish_synthetic_samples(
    uint64_t first,
    uint64_t end_exclusive,
    bool include_three_sample_gap
)
{
    const float channels[SEISMIC_CHANNEL_COUNT] = {1.0f, 2.0f, 3.0f, 4.0f};
    for (uint64_t index = first; index < end_exclusive; ++index) {
        if (include_three_sample_gap && index >= 100U && index <= 102U) {
            continue;
        }
        assert(seismic_buffer_push_indexed(
            (int64_t)index * 2000LL,
            index,
            channels
        ) == ESP_OK);
    }
}


static size_t copy_ring_into_cache(
    uint64_t *cursor,
    seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *sample_indices,
    size_t max_frames,
    uint64_t *dropped_out
)
{
    size_t frame_count = 0U;
    uint64_t dropped = 0U;
    seismic_buffer_info_t info = {0};
    assert(seismic_buffer_copy_since_with_source_indices(
        cursor,
        frames,
        sample_indices,
        max_frames,
        &frame_count,
        &dropped,
        &info
    ) == ESP_OK);
    if (frame_count > 0U) {
        const uint64_t first_ring_frame =
            *cursor - (uint64_t)frame_count + 1U;
        assert(seismic_recorder_cache_append_with_source_indices(
            cache,
            first_ring_frame,
            frames,
            sample_indices,
            frame_count
        ) == ESP_OK);
    }
    if (dropped_out != NULL) {
        *dropped_out = dropped;
    }
    return frame_count;
}


static size_t persist_cache_batch(
    FILE *samples,
    seismic_recorder_cache_t *cache,
    seismic_frame_t *frames,
    uint64_t *sample_indices,
    size_t max_frames
)
{
    size_t total_written = 0U;
    for (;;) {
        size_t frame_count = 0U;
        uint64_t first_session_frame = 0U;
        assert(seismic_recorder_cache_copy_dirty_with_source_indices(
            cache,
            frames,
            sample_indices,
            max_frames,
            &frame_count,
            &first_session_frame
        ) == ESP_OK);
        if (frame_count == 0U) {
            break;
        }

        size_t frames_written = 0U;
        assert(test_fs_write_frames_with_source_indices(
            samples,
            frames,
            sample_indices,
            frame_count,
            &frames_written
        ) == ESP_OK);
        assert(frames_written == frame_count);
        uint64_t last_ring_frame = 0U;
        assert(seismic_recorder_cache_mark_persisted(
            cache,
            first_session_frame,
            frames_written,
            &last_ring_frame
        ) == ESP_OK);
        total_written += frames_written;
    }
    return total_written;
}


static void assert_recorded_timestamps_and_gaps(
    FILE *samples,
    const char *session_id,
    uint64_t logical_frames,
    uint64_t expected_missing,
    uint64_t overflow_cutoff
)
{
    assert(seismic_recorder_fs_close(samples) == ESP_OK);
    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session_id
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    FILE *reader_file = fopen(path, "rb");
    assert(reader_file != NULL);
    assert(fseek(reader_file, 0L, SEEK_END) == 0);
    const long file_size = ftell(reader_file);
    assert(file_size > 0L);

    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = SEISMIC_RECORDER_BINARY_VERSION_V2;
    summary.samples_written = logical_frames;
    summary.complete = true;
    summary.recovered = true;
    seismic_recorder_samples_reader_t reader;
    const esp_err_t open_err = seismic_recorder_samples_reader_open(
        test_file_read_at,
        reader_file,
        (uint64_t)file_size,
        &summary,
        &reader
    );
    assert(open_err == ESP_OK);

    uint64_t logical_position = 0U;
    unsigned gap_blocks = 0U;
    while (logical_position < logical_frames) {
        seismic_frame_t decoded[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
        size_t decoded_count = 0U;
        uint64_t first_index = 0U;
        bool gap_before = false;
        assert(seismic_recorder_samples_reader_next(
            test_file_read_at,
            reader_file,
            &reader,
            decoded,
            SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
            &decoded_count,
            &first_index,
            &gap_before
        ) == ESP_OK);
        assert(decoded_count > 0U);
        if (gap_before) {
            ++gap_blocks;
        }
        for (size_t index = 0U; index < decoded_count; ++index) {
            const uint64_t source_index = first_index + (uint64_t)index;
            assert(decoded[index].timestamp_us == (int64_t)source_index * 2000LL);
            if (overflow_cutoff != UINT64_MAX && logical_position >= overflow_cutoff) {
                assert(source_index == logical_position + expected_missing);
            }
            else if (overflow_cutoff != UINT64_MAX) {
                assert(source_index == logical_position);
            }
            if (expected_missing == 3U && logical_position >= 100U) {
                assert(source_index == logical_position + 3U);
            }
            ++logical_position;
        }
    }
    assert(reader.has_gaps == (expected_missing > 0U));
    assert(reader.missing_sample_count == expected_missing);
    assert(gap_blocks == 1U);
    assert(seismic_recorder_samples_reader_finish(
        test_file_read_at,
        reader_file,
        &reader
    ) == ESP_OK);
    assert(fclose(reader_file) == 0);
}


static void test_acquisition_gap_survives_ring_cache_v2_reader(void)
{
    remove_layout();
    make_layout();
    assert(seismic_buffer_init() == ESP_OK);
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);

    publish_synthetic_samples(0U, 200U, true);
    seismic_recorder_cache_slot_t slots[256];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, slots, 256U) == ESP_OK);
    seismic_frame_t frames[256] = {0};
    uint64_t sample_indices[256] = {0};
    uint64_t cursor = 0U;
    uint64_t dropped = UINT64_MAX;
    const size_t count = copy_ring_into_cache(
        &cursor, &cache, frames, sample_indices, 256U, &dropped
    );
    assert(count == 197U && dropped == 0U);
    assert(persist_cache_batch(samples, &cache, frames, sample_indices, 256U) == 197U);
    assert_recorded_timestamps_and_gaps(samples, session, 197U, 3U, UINT64_MAX);
    remove_layout();
}


static void test_ring_overflow_survives_as_recorded_gap(void)
{
    remove_layout();
    make_layout();
    assert(seismic_buffer_configure(SEISMIC_SOURCE_AD7606, 500U) == ESP_OK);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);

    seismic_recorder_cache_slot_t slots[2200];
    seismic_recorder_cache_t cache;
    assert(seismic_recorder_cache_init(&cache, slots, 2200U) == ESP_OK);
    seismic_frame_t frames[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
    uint64_t sample_indices[SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES] = {0};
    uint64_t cursor = 0U;
    publish_synthetic_samples(0U, 100U, false);
    assert(copy_ring_into_cache(
        &cursor, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES, NULL
    ) == 100U);
    assert(persist_cache_batch(
        samples, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES
    ) == 100U);

    const uint64_t capacity = seismic_buffer_capacity();
    assert(capacity == 2048U);
    publish_synthetic_samples(100U, 2200U, false);
    uint64_t dropped = 0U;
    const size_t retained = copy_ring_into_cache(
        &cursor,
        &cache,
        frames,
        sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
        &dropped
    );
    assert(dropped == 52U && retained == SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES);
    while (cursor < 2200U) {
        const size_t count = copy_ring_into_cache(
            &cursor,
            &cache,
            frames,
            sample_indices,
            SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES,
            NULL
        );
        assert(count > 0U);
    }
    assert(persist_cache_batch(
        samples, &cache, frames, sample_indices,
        SEISMIC_RECORDER_V2_BLOCK_MAX_FRAMES
    ) == 2048U);
    assert_recorded_timestamps_and_gaps(samples, session, 2148U, 52U, 100U);
    remove_layout();
}
#endif


static void test_ad7606_session_writes_v2_blocks_and_source_gaps(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    uint16_t version = 0U;
    assert(test_fs_create_ad7606_session(session, &samples, &version) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(samples != NULL);
    assert(version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    recorder_test_fwrite_calls = 0U;

    const float scale_uv_per_lsb = 5000000.0f / 32768.0f;
    const int16_t raw_samples[4][SEISMIC_CHANNEL_COUNT] = {
        {-32768, -12345, 0, 32767},
        {-100, 1, 12345, 30000},
        {32767, 0, -1, -32768},
        {123, -456, 789, -1011},
    };
    const uint64_t sample_indices[4] = {100U, 101U, 104U, 105U};
    seismic_frame_t frames[4] = {0};
    for (size_t frame = 0U; frame < 4U; ++frame) {
        frames[frame].timestamp_us = 100000LL + (int64_t)frame * 2000LL;
        if (frame >= 2U) {
            frames[frame].timestamp_us += 4000LL;
        }
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            frames[frame].channel_uv[channel] =
                (float)raw_samples[frame][channel] * scale_uv_per_lsb;
        }
    }

    size_t frames_written = 0U;
    assert(test_fs_write_frames_with_source_indices(
        samples,
        frames,
        sample_indices,
        4U,
        &frames_written
    ) == ESP_OK);
    assert(frames_written == 4U);
    assert(recorder_test_fwrite_calls == 1U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char path[256];
    const int path_size = snprintf(
        path,
        sizeof(path),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
        session
    );
    assert(path_size > 0 && (size_t)path_size < sizeof(path));
    samples = fopen(path, "rb");
    assert(samples != NULL);

    seismic_recorder_binary_header_t header;
    seismic_recorder_binary_v2_metadata_t metadata;
    assert(fread(&header, sizeof(header), 1U, samples) == 1U);
    assert(header.version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(header.frame_size_bytes == SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES);
    assert(fread(&metadata, sizeof(metadata), 1U, samples) == 1U);
    assert(fabsf(metadata.scale_uv_per_lsb[0] - scale_uv_per_lsb) < 0.0001f);
    assert(metadata.input_range_v == 5.0f);
    assert(metadata.oversampling_ratio == 4U);
    assert(metadata.spi_clock_hz == 10000000U);

    seismic_recorder_binary_v2_block_header_t first_block;
    seismic_recorder_binary_v2_block_header_t second_block;
    assert(fread(&first_block, sizeof(first_block), 1U, samples) == 1U);
    assert(first_block.first_sample_index == 100U);
    assert(first_block.first_timestamp_us == 100000U);
    assert(first_block.frame_count == 2U);
    assert(first_block.flags == 0U);
    uint8_t first_payload[2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    assert(fread(first_payload, sizeof(first_payload), 1U, samples) == 1U);
    assert(fread(&second_block, sizeof(second_block), 1U, samples) == 1U);
    assert(second_block.first_sample_index == 104U);
    assert(second_block.first_timestamp_us == 108000U);
    assert(second_block.frame_count == 2U);
    assert(second_block.flags == SEISMIC_RECORDER_V2_BLOCK_FLAG_GAP_BEFORE);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const size_t offset =
                frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * 2U;
            assert(test_read_i16_le(&first_payload[offset]) == raw_samples[frame][channel]);
        }
    }
    uint8_t second_payload[2U * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES];
    assert(fread(second_payload, sizeof(second_payload), 1U, samples) == 1U);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            const size_t offset =
                frame * SEISMIC_RECORDER_BINARY_I16_FRAME_BYTES + channel * 2U;
            assert(test_read_i16_le(&second_payload[offset]) == raw_samples[frame + 2U][channel]);
        }
    }

    assert(fseek(samples, 0L, SEEK_END) == 0);
    const long file_size = ftell(samples);
    assert(file_size > 0L);
    seismic_recorder_session_summary_t summary = {0};
    summary.sample_rate_hz = 500U;
    summary.channel_count = SEISMIC_CHANNEL_COUNT;
    summary.format_version = SEISMIC_RECORDER_BINARY_VERSION_V2;
    summary.samples_written = 4U;
    summary.complete = true;
    summary.recovered = true;
    seismic_recorder_samples_reader_t reader;
    assert(seismic_recorder_samples_reader_open(
        test_file_read_at,
        samples,
        (uint64_t)file_size,
        &summary,
        &reader
    ) == ESP_OK);
    seismic_frame_t decoded[4] = {0};
    size_t decoded_count = 0U;
    uint64_t first_index = 0U;
    bool gap_before = true;
    assert(seismic_recorder_samples_reader_next(
        test_file_read_at, samples, &reader, decoded, 4U,
        &decoded_count, &first_index, &gap_before
    ) == ESP_OK);
    assert(decoded_count == 2U && first_index == 100U && !gap_before);
    assert(decoded[0].timestamp_us == 100000LL);
    assert(decoded[1].timestamp_us == 102000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(
                decoded[frame].channel_uv[channel] - frames[frame].channel_uv[channel]
            ) <= scale_uv_per_lsb * 0.5f);
        }
    }
    assert(seismic_recorder_samples_reader_next(
        test_file_read_at, samples, &reader, decoded, 4U,
        &decoded_count, &first_index, &gap_before
    ) == ESP_OK);
    assert(decoded_count == 2U && first_index == 104U && gap_before);
    assert(decoded[0].timestamp_us == 108000LL);
    assert(decoded[1].timestamp_us == 110000LL);
    for (size_t frame = 0U; frame < 2U; ++frame) {
        for (size_t channel = 0U; channel < SEISMIC_CHANNEL_COUNT; ++channel) {
            assert(fabsf(
                decoded[frame].channel_uv[channel] - frames[frame + 2U].channel_uv[channel]
            ) <= scale_uv_per_lsb * 0.5f);
        }
    }
    assert(reader.has_gaps && reader.missing_sample_count == 2U);
    assert(seismic_recorder_samples_reader_finish(test_file_read_at, samples, &reader) == ESP_OK);

    assert(fclose(samples) == 0);
    remove_layout();
}


static void test_v1_and_v2_sessions_share_one_object(void)
{
    remove_layout();
    make_layout();

    char first_session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *samples = NULL;
    assert(test_fs_create_session(
        "OBJ_0001", 500U, SEISMIC_CHANNEL_COUNT,
        first_session, &samples
    ) == ESP_OK);
    seismic_frame_t frame = {
        .timestamp_us = 2000LL,
        .channel_uv = {1.0f, 2.0f, 3.0f, 4.0f},
    };
    size_t frames_written = 0U;
    assert(test_fs_write_frames(samples, &frame, 1U, &frames_written) == ESP_OK);
    assert(frames_written == 1U);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char second_session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    uint16_t format_version = 0U;
    assert(test_fs_create_ad7606_session(
        second_session, &samples, &format_version
    ) == ESP_OK);
    assert(format_version == SEISMIC_RECORDER_BINARY_VERSION_V2);
    assert(seismic_recorder_fs_close(samples) == ESP_OK);

    char path[256];
    const char *const session_ids[] = {first_session, second_session};
    const uint16_t expected_versions[] = {
        SEISMIC_RECORDER_BINARY_VERSION_V1,
        SEISMIC_RECORDER_BINARY_VERSION_V2,
    };
    for (size_t index = 0U; index < 2U; ++index) {
        const int path_size = snprintf(
            path,
            sizeof(path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/%s/samples.bin",
            session_ids[index]
        );
        assert(path_size > 0 && (size_t)path_size < sizeof(path));
        samples = fopen(path, "rb");
        assert(samples != NULL);
        seismic_recorder_binary_header_t header;
        assert(fread(&header, sizeof(header), 1U, samples) == 1U);
        assert(header.version == expected_versions[index]);
        assert(fclose(samples) == 0);
    }
    remove_layout();
}


static void test_large_frame_write_is_chunked_through_staging(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    recorder_test_fwrite_calls = 0U;

    enum { frame_count = SEISMIC_RECORDER_WRITE_STAGING_FRAMES + 1U };
    seismic_frame_t frames[frame_count] = {0};
    for (size_t index = 0U; index < frame_count; ++index) {
        frames[index].timestamp_us = (int64_t)index + 1LL;
        frames[index].channel_uv[0] = (float)index;
    }

    size_t frames_written = 0U;
    assert(seismic_recorder_fs_write_frames(file, frames, frame_count, &frames_written) == ESP_OK);
    assert(frames_written == frame_count);
    /* Each bounded fwrite chunk releases the TFT/SD arbitration window. */
    assert(storage_begin_calls == 3U);
    assert(storage_end_calls == 3U);
    assert(recorder_test_fwrite_calls == 2U);
    assert(seismic_recorder_fs_close(file) == ESP_OK);

    file = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin", "rb");
    assert(file != NULL);
    assert(fseek(file, (long)sizeof(seismic_recorder_binary_header_t), SEEK_SET) == 0);
    seismic_frame_t last = {0};
    assert(fseek(file, (long)(sizeof(seismic_frame_t) * (frame_count - 1U)), SEEK_CUR) == 0);
    assert(fread(&last, sizeof(last), 1U, file) == 1U);
    assert(last.timestamp_us == (int64_t)frame_count);
    assert(last.channel_uv[0] == (float)(frame_count - 1U));
    assert(fclose(file) == 0);
    remove_layout();
}

static void test_session_error_cleanup(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_ERR_INVALID_STATE;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = "stale";
    FILE *file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_ERR_INVALID_STATE);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 0U);

    storage_begin_error = ESP_OK;
    fail_create_directory = true;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_FAIL);
    assert(session[0] != '\0');
    assert(file == NULL);
    assert(storage_begin_calls == 2U);
    assert(storage_end_calls == 1U);
    assert(exclusive_directory_calls == 1U);
    assert(remove_tree_calls == 0U);

    fail_create_directory = false;
    remove_layout();
}

static void test_session_cleanup_after_post_mkdir_failure(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';
    fake_remove_tree_error = ESP_ERR_INVALID_STATE;
    injected_short_write_count = SIZE_MAX;
    injected_fflush_error = true;
    injected_fclose_error = false;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(
        seismic_recorder_fs_create_session(
            "OBJ_0001",
            500,
            4,
            session,
            &file
        ) == ESP_FAIL
    );
    assert(strcmp(session, "S000001") == 0);
    assert(file == NULL);
    assert(exclusive_directory_calls == 1U);
    assert(remove_tree_calls == 1U);
    assert(
        strcmp(
            removed_tree_path,
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001"
        ) == 0
    );
    assert(storage_begin_calls == 1U);
    assert(storage_end_calls == 1U);

    fake_remove_tree_error = ESP_OK;
    injected_fflush_error = false;
    remove_layout();
}

static void test_writer_errors_are_propagated(void)
{
    remove_layout();
    make_layout();
    storage_begin_error = ESP_OK;
    injected_short_write_count = SIZE_MAX;
    injected_fflush_error = false;
    injected_fsync_error = false;
    injected_fclose_error = false;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    seismic_frame_t frames[3] = {0};
    size_t frames_written = 99U;

    injected_short_write_count = 1U;
    assert(seismic_recorder_fs_write_frames(file, frames, 3U, &frames_written) == ESP_FAIL);
    assert(frames_written == 1U);
    injected_short_write_count = SIZE_MAX;

    injected_fflush_error = true;
    assert(seismic_recorder_fs_flush(file, false) == ESP_FAIL);
    injected_fflush_error = false;

    injected_fsync_error = true;
    assert(seismic_recorder_fs_sync(file) == ESP_FAIL);
    injected_fsync_error = false;

    injected_fclose_error = true;
    assert(seismic_recorder_fs_close(file) == ESP_FAIL);
    injected_fclose_error = false;
    remove_layout();
}

static void test_session_id_increments(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007") == 0);
    FILE *marker = fopen(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007/marker", "wb");
    assert(marker != NULL);
    assert(fclose(marker) == 0);

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000008") == 0);
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    (void)remove(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007/marker");
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007");
    remove_layout();
}


static void test_staged_artifacts_attach_once_to_the_created_session(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        assert(!path_exists(staged_path));
        assert_file_contents(session_path, staged_artifact_contents[i]);
    }
    assert(seismic_recorder_fs_close(file) == ESP_OK);

    memset(session, 0, sizeof(session));
    file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000002") == 0);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char session_path[256], staged_path[256];
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        assert(!path_exists(session_path));
        assert(!path_exists(staged_path));
    }
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_missing_staging_is_a_noop(void)
{
    remove_layout();
    make_layout();

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(!path_exists(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging"));
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_one_staged_move_failure_does_not_fail_recording(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();
    (void)snprintf(
        injected_rename_source,
        sizeof(injected_rename_source),
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/preview-v1.bin"
    );

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_OK);
    assert(strcmp(session, "S000001") == 0);
    assert(file != NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        if (strcmp(staged_artifact_names[i], "preview-v1.bin") == 0) {
            assert(path_exists(staged_path));
            assert(!path_exists(session_path));
            assert_file_contents(staged_path, staged_artifact_contents[i]);
        }
        else {
            assert(!path_exists(staged_path));
            assert_file_contents(session_path, staged_artifact_contents[i]);
        }
    }
    assert(injected_rename_source[0] == '\0');
    assert(seismic_recorder_fs_close(file) == ESP_OK);
    remove_layout();
}


static void test_failed_start_restores_staged_artifacts(void)
{
    remove_layout();
    make_layout();
    write_staged_artifacts();
    injected_fflush_error = true;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = {0};
    FILE *file = NULL;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    injected_fflush_error = false;
    assert(strcmp(session, "S000001") == 0);
    assert(file == NULL);
    for (size_t i = 0U; i < sizeof(staged_artifact_names) / sizeof(staged_artifact_names[0]); ++i) {
        char staged_path[256], session_path[256];
        (void)snprintf(
            staged_path,
            sizeof(staged_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/staging/%s",
            staged_artifact_names[i]
        );
        (void)snprintf(
            session_path,
            sizeof(session_path),
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/%s",
            staged_artifact_names[i]
        );
        assert_file_contents(staged_path, staged_artifact_contents[i]);
        assert(!path_exists(session_path));
    }
    remove_layout();
}


static void test_create_session_aborts_on_session_scan_error(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);

    const char *samples_path =
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/samples.bin";
    FILE *sentinel = fopen(samples_path, "wb");
    assert(sentinel != NULL);
    assert(fputs("preserved", sentinel) >= 0);
    assert(fclose(sentinel) == 0);

    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    reset_directory_fault();
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;

    char session[SEISMIC_RECORDER_SESSION_ID_SIZE] = "stale";
    FILE *file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(exclusive_directory_calls == 0U);
    assert(remove_tree_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    sentinel = fopen(samples_path, "rb");
    assert(sentinel != NULL);
    char contents[16] = {0};
    assert(fread(contents, 1U, sizeof(contents) - 1U, sentinel) == 9U);
    assert(strcmp(contents, "preserved") == 0);
    assert(fclose(sentinel) == 0);

    reset_directory_fault();
    exclusive_directory_calls = 0U;
    remove_tree_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    memset(session, 0, sizeof(session));
    file = (FILE *)1;
    assert(seismic_recorder_fs_create_session("OBJ_0001", 500, 4, session, &file) == ESP_FAIL);
    assert(session[0] == '\0');
    assert(file == NULL);
    assert(exclusive_directory_calls == 0U);
    assert(remove_tree_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    remove_layout();
}


static void test_listing_missing_and_malformed_metadata_is_safe(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003") == 0);
    /* A gap must not cause presumed slot probes; only directory entries count. */
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000007") == 0);

    seismic_recorder_session_summary_t sessions[4] = {0};
    size_t count = 0U;
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fake_summary_error = ESP_ERR_NOT_FOUND;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 0U);
    assert(strcmp(sessions[0].session_id, "S000007") == 0);
    assert(strcmp(sessions[1].session_id, "S000003") == 0);
    assert(strcmp(sessions[2].session_id, "S000001") == 0);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
        assert(sessions[index].source_name[0] == '\0');
    }

    const char *session_ids[] = {"S000001", "S000003", "S000007"};
    for (size_t index = 0U; index < sizeof(session_ids) / sizeof(session_ids[0]); ++index) {
        char metadata_path[256];
        (void)snprintf(
            metadata_path,
            sizeof(metadata_path),
            "%s/OBJ_0001/sessions/%s/session.json",
            STORAGE_MANAGER_OBJECTS_PATH,
            session_ids[index]
        );
        FILE *metadata = fopen(metadata_path, "wb");
        assert(metadata != NULL);
        assert(fclose(metadata) == 0);
    }

    fake_summary_error = ESP_ERR_INVALID_RESPONSE;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
    }

    fake_summary_error = ESP_OK;
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 0U);
    assert(fake_known_present_summary_read_calls == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(sessions[index].complete);
        assert(strcmp(sessions[index].source_name, "AD7606") == 0);
    }

    /* A backup-only session must retain the normal recovery path. */
    assert(
        remove(
            STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003/session.json"
        ) == 0
    );
    FILE *backup = fopen(
        STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003/session.json.bak",
        "wb"
    );
    assert(backup != NULL);
    assert(fclose(backup) == 0);
    fake_summary_read_calls = 0U;
    fake_known_present_summary_read_calls = 0U;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    assert(fake_summary_read_calls == 1U);
    assert(fake_known_present_summary_read_calls == 2U);

    assert(storage_begin_calls == storage_end_calls);
    fake_summary_error = ESP_FAIL;
    remove_layout();
}


static void test_listing_propagates_directory_errors(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);

    seismic_recorder_session_summary_t sessions[2] = {0};
    size_t count = 0U;
    storage_begin_error = ESP_OK;
    fake_summary_error = ESP_OK;

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EACCES;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_opendir_calls == 1U);
    assert(directory_closedir_calls == 0U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_OPENDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EACCES;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_opendir_calls == 2U);
    assert(directory_closedir_calls == 1U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_readdir_calls == 1U);
    assert(directory_closedir_calls == 1U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_READDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_readdir_calls == 2U);
    /* The injected second read can hit the parent census before a child
     * directory is opened (for example when '.'/'..' are returned first).
     * The invariant is leak freedom, not a platform-specific close count. */
    assert(directory_closedir_calls == directory_opendir_calls);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 1U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 0U);
    assert(directory_closedir_calls == 2U);
    assert(storage_begin_calls == storage_end_calls);

    reset_directory_fault();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    directory_fault = SEISMIC_RECORDER_DIRECTORY_FAULT_CLOSEDIR;
    directory_fault_call = 2U;
    directory_fault_errno = EIO;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 2U, &count) == ESP_FAIL);
    assert(count == 1U);
    assert(!sessions[0].complete);
    assert(directory_closedir_calls == 2U);
    assert(storage_begin_calls == storage_end_calls);

    fake_summary_error = ESP_FAIL;
    reset_directory_fault();
    remove_layout();
}

int main(void)
{
    test_invalid_arguments();
    test_session_id_parser_is_canonical();
    test_session_header_and_frames();
    test_ad7606_session_writes_v2_blocks_and_source_gaps();
    test_v1_and_v2_sessions_share_one_object();
    test_large_frame_write_is_chunked_through_staging();
    test_session_error_cleanup();
    test_session_cleanup_after_post_mkdir_failure();
    test_writer_errors_are_propagated();
    test_session_id_increments();
    test_staged_artifacts_attach_once_to_the_created_session();
    test_missing_staging_is_a_noop();
    test_one_staged_move_failure_does_not_fail_recording();
    test_failed_start_restores_staged_artifacts();
    test_create_session_aborts_on_session_scan_error();
    test_listing_missing_and_malformed_metadata_is_safe();
    test_listing_propagates_directory_errors();
#if defined(SEISMIC_RECORDER_GAP_PIPELINE_TEST)
    test_acquisition_gap_survives_ring_cache_v2_reader();
    test_ring_overflow_survives_as_recorded_gap();
#endif
    test_session_range_v1_preserves_timestamps_and_bounds();
    test_session_range_v2_seeks_blocks_and_preserves_gaps();
    return 0;
}
