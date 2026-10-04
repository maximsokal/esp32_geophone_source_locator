#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

#include "object_recovery_internal.h"
#include "seismic_recorder.h"
#include "storage_manager.h"


static seismic_recorder_session_summary_t s_sessions[SEISMIC_RECORDER_MAX_SESSIONS];
static size_t s_session_count;
static esp_err_t s_list_sessions_result;
static unsigned s_operation_begin_calls;
static unsigned s_operation_end_calls;
static unsigned s_calloc_calls;
static size_t s_last_calloc_count;
static size_t s_last_calloc_size;
static bool s_last_calloc_succeeded;
static bool s_fail_next_calloc;


void *object_recovery_test_calloc(size_t count, size_t size)
{
    ++s_calloc_calls;
    s_last_calloc_count = count;
    s_last_calloc_size = size;
    s_last_calloc_succeeded = false;

    if (s_fail_next_calloc) {
        s_fail_next_calloc = false;
        return NULL;
    }
    if (count != 0U && size > (size_t)-1 / count) {
        return NULL;
    }

    const size_t bytes = count * size;
    void *allocation = malloc(bytes);
    if (allocation != NULL) {
        memset(allocation, 0, bytes);
        s_last_calloc_succeeded = true;
    }
    return allocation;
}


esp_err_t storage_manager_operation_begin(void)
{
    ++s_operation_begin_calls;
    return ESP_OK;
}


void storage_manager_operation_end(void)
{
    ++s_operation_end_calls;
}


esp_err_t seismic_recorder_list_sessions(
    const char *object_id,
    seismic_recorder_session_summary_t *sessions,
    size_t max_sessions,
    size_t *session_count
)
{
    (void)object_id;
    if (s_list_sessions_result != ESP_OK) {
        return s_list_sessions_result;
    }

    const size_t count = s_session_count < max_sessions ? s_session_count : max_sessions;
    for (size_t i = 0U; i < count; ++i) {
        sessions[i] = s_sessions[i];
    }
    *session_count = count;
    return ESP_OK;
}


static void set_sessions(const char *const *session_ids, size_t count)
{
    s_session_count = count;
    for (size_t i = 0U; i < count; ++i) {
        memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
        (void)snprintf(s_sessions[i].session_id, sizeof(s_sessions[i].session_id), "%s", session_ids[i]);
    }
    s_list_sessions_result = ESP_OK;
}


static void object_path(char *path, size_t path_size, const char *object_id)
{
    (void)snprintf(path, path_size, "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id);
}


static void legacy_scan_path(char *path, size_t path_size, const char *object_id)
{
    (void)snprintf(path, path_size, "%s/%s/scan-v1.bin", STORAGE_MANAGER_OBJECTS_PATH, object_id);
}


static void session_scan_path(char *path, size_t path_size, const char *object_id, const char *session_id)
{
    (void)snprintf(path, path_size, "%s/%s/sessions/%s/scan-v1.bin", STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
}


static void write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}


static bool file_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}


static void ensure_dir(const char *path)
{
    (void)mkdir(path, 0775);
}


static void prepare_object_dirs(const char *object_id, const char *const *session_ids, size_t session_count)
{
    ensure_dir("object_recovery_legacy_scan_tmp");
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);

    char path[256];
    object_path(path, sizeof(path), object_id);
    ensure_dir(path);

    (void)snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);

    for (size_t i = 0U; i < session_count; ++i) {
        (void)snprintf(path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id, session_ids[i]);
        ensure_dir(path);
    }
}


static void cleanup(const char *object_id, const char *const *session_ids, size_t session_count)
{
    char path[256];
    for (size_t i = 0U; i < session_count; ++i) {
        session_scan_path(path, sizeof(path), object_id, session_ids[i]);
        (void)remove(path);
    }
    legacy_scan_path(path, sizeof(path), object_id);
    (void)remove(path);
}


static void test_rejects_null_object_id(void)
{
    assert(object_recovery_migrate_legacy_scan(NULL) == ESP_ERR_INVALID_ARG);
}


static void test_no_legacy_file_is_a_silent_no_op(void)
{
    const char *sessions[] = { "S000001" };
    prepare_object_dirs("OBJ_0001", sessions, 1U);
    cleanup("OBJ_0001", sessions, 1U);
    set_sessions(sessions, 1U);

    assert(object_recovery_migrate_legacy_scan("OBJ_0001") == ESP_OK);

    char destination[256];
    session_scan_path(destination, sizeof(destination), "OBJ_0001", "S000001");
    assert(!file_exists(destination));
}


static void test_no_sessions_leaves_legacy_file_in_place(void)
{
    prepare_object_dirs("OBJ_0002", NULL, 0U);
    cleanup("OBJ_0002", NULL, 0U);
    set_sessions(NULL, 0U);

    char legacy[256];
    legacy_scan_path(legacy, sizeof(legacy), "OBJ_0002");
    write_file(legacy, "legacy-scan-bytes");

    assert(object_recovery_migrate_legacy_scan("OBJ_0002") == ESP_OK);
    assert(file_exists(legacy));

    (void)remove(legacy);
}


static void test_migrates_to_the_newest_session(void)
{
    const char *sessions[] = { "S000001", "S000003", "S000002" };
    prepare_object_dirs("OBJ_0003", sessions, 3U);
    cleanup("OBJ_0003", sessions, 3U);
    set_sessions(sessions, 3U);

    char legacy[256];
    legacy_scan_path(legacy, sizeof(legacy), "OBJ_0003");
    write_file(legacy, "legacy-scan-bytes");

    s_calloc_calls = 0U;
    assert(object_recovery_migrate_legacy_scan("OBJ_0003") == ESP_OK);
    assert(s_calloc_calls == 1U);
    assert(s_last_calloc_count == SEISMIC_RECORDER_MAX_SESSIONS);
    assert(s_last_calloc_size == sizeof(*s_sessions));
    assert(s_last_calloc_succeeded);
    assert(!file_exists(legacy));

    char destination[256];
    /* S000003 is lexicographically (and numerically) the newest of the
     * three, regardless of the order sessions were reported in. */
    session_scan_path(destination, sizeof(destination), "OBJ_0003", "S000003");
    assert(file_exists(destination));

    FILE *file = fopen(destination, "rb");
    assert(file != NULL);
    char contents[32] = {0};
    const size_t read_count = fread(contents, 1, sizeof(contents) - 1, file);
    assert(fclose(file) == 0);
    assert(strcmp(contents, "legacy-scan-bytes") == 0);
    (void)read_count;

    cleanup("OBJ_0003", sessions, 3U);
}


static void test_allocation_failure_returns_no_mem_and_ends_operation(void)
{
    const char *sessions[] = { "S000001" };
    prepare_object_dirs("OBJ_0006", sessions, 1U);
    cleanup("OBJ_0006", sessions, 1U);
    set_sessions(sessions, 1U);

    char legacy[256];
    legacy_scan_path(legacy, sizeof(legacy), "OBJ_0006");
    write_file(legacy, "legacy-scan-bytes");

    const unsigned begin_calls_before = s_operation_begin_calls;
    const unsigned end_calls_before = s_operation_end_calls;
    s_calloc_calls = 0U;
    s_fail_next_calloc = true;

    assert(object_recovery_migrate_legacy_scan("OBJ_0006") == ESP_ERR_NO_MEM);
    assert(s_calloc_calls == 1U);
    assert(s_last_calloc_count == SEISMIC_RECORDER_MAX_SESSIONS);
    assert(s_last_calloc_size == sizeof(*s_sessions));
    assert(!s_last_calloc_succeeded);
    assert(!s_fail_next_calloc);
    assert(s_operation_begin_calls == begin_calls_before + 1U);
    assert(s_operation_end_calls == end_calls_before + 1U);
    assert(file_exists(legacy));

    cleanup("OBJ_0006", sessions, 1U);
}


static void test_never_overwrites_an_existing_session_scan(void)
{
    const char *sessions[] = { "S000001" };
    prepare_object_dirs("OBJ_0004", sessions, 1U);
    cleanup("OBJ_0004", sessions, 1U);
    set_sessions(sessions, 1U);

    char legacy[256];
    legacy_scan_path(legacy, sizeof(legacy), "OBJ_0004");
    write_file(legacy, "legacy-bytes");

    char destination[256];
    session_scan_path(destination, sizeof(destination), "OBJ_0004", "S000001");
    write_file(destination, "already-there");

    assert(object_recovery_migrate_legacy_scan("OBJ_0004") == ESP_OK);
    /* Neither file is touched: the legacy copy stays exactly where it was,
     * and the pre-existing session-owned scan is not clobbered. */
    assert(file_exists(legacy));

    FILE *file = fopen(destination, "rb");
    assert(file != NULL);
    char contents[32] = {0};
    assert(fread(contents, 1, sizeof(contents) - 1, file) > 0);
    assert(fclose(file) == 0);
    assert(strcmp(contents, "already-there") == 0);

    cleanup("OBJ_0004", sessions, 1U);
}


static void test_session_list_failure_leaves_legacy_file_in_place(void)
{
    prepare_object_dirs("OBJ_0005", NULL, 0U);
    cleanup("OBJ_0005", NULL, 0U);
    s_session_count = 0U;
    s_list_sessions_result = ESP_FAIL;

    char legacy[256];
    legacy_scan_path(legacy, sizeof(legacy), "OBJ_0005");
    write_file(legacy, "legacy-scan-bytes");

    assert(object_recovery_migrate_legacy_scan("OBJ_0005") == ESP_OK);
    assert(file_exists(legacy));

    (void)remove(legacy);
    s_list_sessions_result = ESP_OK;
}


int main(void)
{
    test_rejects_null_object_id();
    test_no_legacy_file_is_a_silent_no_op();
    test_no_sessions_leaves_legacy_file_in_place();
    test_migrates_to_the_newest_session();
    test_allocation_failure_returns_no_mem_and_ends_operation();
    test_never_overwrites_an_existing_session_scan();
    test_session_list_failure_leaves_legacy_file_in_place();
    return 0;
}
