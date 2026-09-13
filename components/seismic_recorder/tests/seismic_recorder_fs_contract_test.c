/*
 * This test includes the production filesystem translation unit so the host
 * binary exercises its real header/session/path code. Storage and object
 * boundaries are faked below; no user SD path is touched.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
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
static size_t fake_summary_read_calls;
static size_t fake_known_present_summary_read_calls;
static size_t injected_short_write_count = SIZE_MAX;
static bool injected_fflush_error;
static bool injected_fsync_error;
static bool injected_fclose_error;

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
            (void)snprintf(session_path, sizeof(session_path), "%s/%s", sessions_path, entry->d_name);
            (void)snprintf(samples_path, sizeof(samples_path), "%s/samples.bin", session_path);
            (void)snprintf(marker_path, sizeof(marker_path), "%s/marker", session_path);
            (void)snprintf(metadata_path, sizeof(metadata_path), "%s/session.json", session_path);
            (void)snprintf(backup_path, sizeof(backup_path), "%s/session.json.bak", session_path);
            (void)remove(samples_path);
            (void)remove(marker_path);
            (void)remove(metadata_path);
            (void)remove(backup_path);
            (void)rmdir(session_path);
        }
        (void)closedir(sessions);
    }
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
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
#define opendir seismic_recorder_test_opendir
#define readdir seismic_recorder_test_readdir
#define closedir seismic_recorder_test_closedir
#include "../seismic_recorder_fs.c"
#undef fwrite
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
#undef fclose
#undef opendir
#undef readdir
#undef closedir

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
    test_large_frame_write_is_chunked_through_staging();
    test_session_error_cleanup();
    test_session_cleanup_after_post_mkdir_failure();
    test_writer_errors_are_propagated();
    test_session_id_increments();
    test_create_session_aborts_on_session_scan_error();
    test_listing_missing_and_malformed_metadata_is_safe();
    test_listing_propagates_directory_errors();
    return 0;
}
