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
static unsigned remove_tree_calls;
static char removed_tree_path[256];
static bool fake_metadata_present;
static esp_err_t fake_summary_error = ESP_FAIL;
static size_t injected_short_write_count = SIZE_MAX;
static bool injected_fflush_error;
static bool injected_fsync_error;
static bool injected_fclose_error;

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
            (void)snprintf(session_path, sizeof(session_path), "%s/%s", sessions_path, entry->d_name);
            (void)snprintf(samples_path, sizeof(samples_path), "%s/samples.bin", session_path);
            (void)snprintf(marker_path, sizeof(marker_path), "%s/marker", session_path);
            (void)remove(samples_path);
            (void)remove(marker_path);
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

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++remove_tree_calls;
    (void)snprintf(removed_tree_path, sizeof(removed_tree_path), "%s", path);
    return ESP_OK;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    (void)object_id;
    (void)profile;
    return ESP_OK;
}

/* Not reached by these tests, but required when this TU includes fs.c. */
esp_err_t seismic_recorder_json_read_summary(
    const char *object_id,
    const char *session_id,
    seismic_recorder_session_summary_t *summary
)
{
    (void)object_id;
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

bool seismic_recorder_json_metadata_exists(
    const char *object_id,
    const char *session_id
)
{
    (void)object_id;
    (void)session_id;
    return fake_metadata_present;
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
#include "../seismic_recorder_fs.c"
#undef fwrite
#undef fflush
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
#undef fclose

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

static void test_session_header_and_frames(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0;
    storage_end_calls = 0;
    storage_begin_error = ESP_OK;
    fail_create_directory = false;
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

static void test_session_error_cleanup(void)
{
    remove_layout();
    make_layout();
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_ERR_INVALID_STATE;
    fail_create_directory = false;
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
    remove_tree_calls = 0U;
    removed_tree_path[0] = '\0';
    assert(seismic_recorder_fs_create_session("OBJ_0001", 50, 4, session, &file) == ESP_FAIL);
    assert(session[0] != '\0');
    assert(file == NULL);
    assert(storage_begin_calls == 2U);
    assert(storage_end_calls == 1U);
    assert(remove_tree_calls == 1U);
    assert(strcmp(removed_tree_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);

    fail_create_directory = false;
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


static void test_listing_missing_and_malformed_metadata_is_safe(void)
{
    remove_layout();
    make_layout();
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000002") == 0);
    assert(make_directory(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000003") == 0);

    seismic_recorder_session_summary_t sessions[4] = {0};
    size_t count = 0U;
    storage_begin_calls = 0U;
    storage_end_calls = 0U;
    storage_begin_error = ESP_OK;
    fake_metadata_present = false;
    fake_summary_error = ESP_OK;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
        assert(sessions[index].source_name[0] == '\0');
    }

    fake_metadata_present = true;
    fake_summary_error = ESP_ERR_INVALID_RESPONSE;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(!sessions[index].complete);
    }

    fake_summary_error = ESP_OK;
    count = 0U;
    assert(seismic_recorder_fs_list_sessions("OBJ_0001", sessions, 4U, &count) == ESP_OK);
    assert(count == 3U);
    for (size_t index = 0U; index < count; ++index) {
        assert(sessions[index].complete);
        assert(strcmp(sessions[index].source_name, "AD7606") == 0);
    }
    assert(storage_begin_calls == storage_end_calls);
    fake_metadata_present = false;
    fake_summary_error = ESP_FAIL;
    remove_layout();
}

int main(void)
{
    test_invalid_arguments();
    test_session_header_and_frames();
    test_session_error_cleanup();
    test_writer_errors_are_propagated();
    test_session_id_increments();
    test_listing_missing_and_malformed_metadata_is_safe();
    return 0;
}
