#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#endif

#include "object_recovery.h"
#include "object_recovery_fs.h"
#include "object_recovery_internal.h"
#include "object_store.h"
#include "storage_manager.h"
#include "cjson_alloc_guard.h"

/* Fault injection behind the OBJECT_RECOVERY_TEST_FAULTS seams. */
extern int cJSON_stub_fail_alloc_countdown;

static struct {
    int malloc_fail_after;        /* -1 never; N = fail after N more mallocs */
    const char *fopen_fail_suffix; /* fail fopen of paths ending with this */
    int fopen_fail_errno;
    unsigned rename_calls;
    unsigned rename_fail_call;    /* 1-based; 0 never */
} s_fault = { .malloc_fail_after = -1 };

static void clear_faults(void)
{
    s_fault.malloc_fail_after = -1;
    s_fault.fopen_fail_suffix = NULL;
    s_fault.fopen_fail_errno = 0;
    s_fault.rename_calls = 0U;
    s_fault.rename_fail_call = 0U;
    cJSON_stub_fail_alloc_countdown = -1;
}

void *object_recovery_test_malloc(size_t size)
{
    if (s_fault.malloc_fail_after == 0) {
        s_fault.malloc_fail_after = -1;
        return NULL;
    }
    if (s_fault.malloc_fail_after > 0) {
        --s_fault.malloc_fail_after;
    }
    return malloc(size);
}

FILE *object_recovery_test_fopen(const char *path, const char *mode)
{
    if (s_fault.fopen_fail_suffix != NULL) {
        const size_t path_length = strlen(path);
        const size_t suffix_length = strlen(s_fault.fopen_fail_suffix);
        if (
            path_length >= suffix_length &&
            strcmp(path + path_length - suffix_length, s_fault.fopen_fail_suffix) == 0
        ) {
            errno = s_fault.fopen_fail_errno;
            return NULL;
        }
    }
    return fopen(path, mode);
}

int object_recovery_test_rename(const char *source, const char *destination)
{
    ++s_fault.rename_calls;
    if (s_fault.rename_fail_call != 0U && s_fault.rename_calls == s_fault.rename_fail_call) {
        errno = EIO;
        return -1;
    }
    return rename(source, destination);
}

static const char *TEST_ROOT = "object_recovery_contract_tmp";

static esp_err_t s_active_read_result = ESP_ERR_NOT_FOUND;
static const char *s_active_marker = NULL;
bool object_recovery_test_object_leased;
static esp_err_t s_remove_result = ESP_OK;
static unsigned s_remove_tree_calls;
static char s_removed_path[256];
static object_store_delete_scope_t s_delete_scope = {
    .sessions = 2U,
    .artifacts = 3U,
    .localization_runs = 1U,
    .bytes = 12345U,
};
static esp_err_t s_delete_scope_result = ESP_OK;
static char s_log_buffer[8192];
static unsigned s_removal_log_count;

unsigned esp_log_timestamp(void)
{
    return 0U;
}

int64_t esp_timer_get_time(void)
{
    return 123456789LL;
}

uint32_t esp_random(void)
{
    return 0x484U;
}

void object_recovery_test_log(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) return;
    const size_t current = strlen(s_log_buffer);
    if ((size_t)written + current + 2U < sizeof(s_log_buffer)) {
        memcpy(s_log_buffer + current, line, (size_t)written);
        s_log_buffer[current + (size_t)written] = '\n';
        s_log_buffer[current + (size_t)written + 1U] = '\0';
    }
    if (strstr(line, "OBJECT_RECOVERY_REMOVED") != NULL) {
        ++s_removal_log_count;
    }
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    ++s_remove_tree_calls;
    snprintf(s_removed_path, sizeof(s_removed_path), "%s", path != NULL ? path : "");
    return s_remove_result;
}

esp_err_t object_store_get_delete_scope(
    const char *object_id,
    object_store_delete_scope_t *scope
)
{
    if (object_id == NULL || scope == NULL) return ESP_ERR_INVALID_ARG;
    if (s_delete_scope_result != ESP_OK) return s_delete_scope_result;
    *scope = s_delete_scope;
    return ESP_OK;
}
esp_err_t storage_manager_read_text(
    const char *path,
    char *buffer,
    size_t buffer_size
)
{
    (void)path;
    if (s_active_read_result != ESP_OK) {
        return s_active_read_result;
    }
    if (s_active_marker == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(buffer, buffer_size, "%s", s_active_marker);
    if (written < 0 || (size_t)written >= buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}


static void test_scan_rejects_invalid_object_ids(void)
{
    bool removed = true;
    assert(object_recovery_check_one(NULL, &removed) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_check_one("OBJ_0001/..", &removed) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_check_one("OBJ_001", &removed) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_check_one("OBJ_0001", NULL) == ESP_ERR_INVALID_ARG);
}

static void test_background_delete_guard(void)
{
    s_active_read_result = ESP_ERR_NOT_FOUND;
    s_active_marker = NULL;
    assert(object_recovery_background_delete_guard("OBJ_0001") == ESP_OK);

    s_active_read_result = ESP_OK;
    s_active_marker = "OBJ_0001\n";
    assert(object_recovery_background_delete_guard("OBJ_0001") == ESP_ERR_INVALID_STATE);
    assert(object_recovery_background_delete_guard("OBJ_0002") == ESP_OK);

    s_active_marker = "not-an-object";
    assert(object_recovery_background_delete_guard("OBJ_0001") == ESP_OK);

    s_active_read_result = ESP_FAIL;
    s_active_marker = NULL;
    assert(object_recovery_background_delete_guard("OBJ_0001") == ESP_FAIL);

    assert(object_recovery_background_delete_guard("OBJ_bad") == ESP_ERR_INVALID_ARG);

    s_active_read_result = ESP_ERR_NOT_FOUND;
    s_active_marker = NULL;
}

static void metadata_path(char *path, size_t path_size, const char *name)
{
    (void)snprintf(path, path_size, "%s/%s", TEST_ROOT, name);
}

static void remove_metadata(const char *name)
{
    char path[128];
    metadata_path(path, sizeof(path), name);
    (void)remove(path);
}

static void remove_test_files(void)
{
    char source[128];
    char destination[128];
    (void)snprintf(source, sizeof(source), "%s/source.txt", TEST_ROOT);
    (void)snprintf(destination, sizeof(destination), "%s/destination.txt", TEST_ROOT);
    remove_metadata("object.json");
    remove_metadata("object.json.bak");
    remove_metadata("object.json.tmp");
    remove_metadata("object.json.old");
    (void)remove(source);
    (void)remove(destination);
    (void)rmdir(TEST_ROOT);
}

static void write_metadata(const char *name, const char *text)
{
    char path[128];
    metadata_path(path, sizeof(path), name);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static bool metadata_exists(const char *name)
{
    char path[128];
    metadata_path(path, sizeof(path), name);
    struct stat info;
    return stat(path, &info) == 0;
}

static void test_metadata_recovery_priority(void)
{
    bool valid = false;
    bool recovered = false;
    object_recovery_metadata_attempts_t attempts = {0};

    remove_test_files();
    assert(mkdir(TEST_ROOT, 0775) == 0);
    write_metadata("object.json", "{\"schema_version\":1,\"id\":\"OBJ_0001\"}");
    write_metadata("object.json.tmp", "stale");
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(valid);
    assert(!recovered);
    /* A valid primary is authoritative. The stale sidecar is deliberately
     * left untouched on the fast path; atomic writes remove it before the
     * next transaction, while fallback recovery still cleans it below. */
    assert(metadata_exists("object.json.tmp"));
    assert(metadata_exists("object.json"));

    remove_metadata("object.json");
    write_metadata("object.json.bak", "{\"schema_version\":1,\"id\":\"OBJ_0001\"}");
    valid = false;
    recovered = false;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(valid);
    assert(recovered);
    assert(metadata_exists("object.json"));
    assert(!metadata_exists("object.json.bak"));
    assert(!metadata_exists("object.json.tmp"));

    remove_metadata("object.json");
    write_metadata("object.json.tmp", "{\"schema_version\":1,\"id\":\"OBJ_0001\"}");
    valid = false;
    recovered = false;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(valid);
    assert(recovered);
    assert(metadata_exists("object.json"));
    assert(!metadata_exists("object.json.tmp"));
    assert(!metadata_exists("object.json.bak"));

    remove_metadata("object.json");
    write_metadata("object.json", "not-json");
    valid = true;
    recovered = true;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(!valid);
    assert(!recovered);
    assert(strcmp(attempts.primary, "invalid_json") == 0);
    assert(strcmp(attempts.backup, "not_found") == 0);
    assert(strcmp(attempts.temporary, "not_found") == 0);

    remove_test_files();
}

static void test_path_builder(void)
{
    char path[64];
    assert(object_recovery_fs_build_path(path, sizeof(path), "/sdcard/objects/OBJ_0001", "object.json") == ESP_OK);
    assert(strcmp(path, "/sdcard/objects/OBJ_0001/object.json") == 0);

    assert(object_recovery_fs_build_path(NULL, sizeof(path), "/x", "f") == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_build_path(path, 0, "/x", "f") == ESP_ERR_INVALID_SIZE);
    assert(object_recovery_fs_build_path(path, sizeof(path), NULL, "f") == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_build_path(path, sizeof(path), "/x", NULL) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_build_path(path, 8, "/long-directory", "file") == ESP_ERR_INVALID_SIZE);
}

static void test_unlink_and_restore(void)
{
    char source[128];
    char destination[128];
    (void)snprintf(source, sizeof(source), "%s/source.txt", TEST_ROOT);
    (void)snprintf(destination, sizeof(destination), "%s/destination.txt", TEST_ROOT);

    remove_test_files();
    (void)mkdir(TEST_ROOT, 0775);

    assert(object_recovery_fs_unlink_if_exists(NULL) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_restore_file(NULL, destination) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_restore_file(source, NULL) == ESP_ERR_INVALID_ARG);
    assert(object_recovery_fs_unlink_if_exists(destination) == ESP_OK);

    FILE *file = fopen(source, "wb");
    assert(file != NULL);
    assert(fputs("recovered", file) >= 0);
    assert(fclose(file) == 0);

    file = fopen(destination, "wb");
    assert(file != NULL);
    assert(fputs("stale", file) >= 0);
    assert(fclose(file) == 0);

    assert(object_recovery_fs_restore_file(source, destination) == ESP_OK);
    file = fopen(destination, "rb");
    assert(file != NULL);
    char contents[16] = {0};
    assert(fread(contents, 1, sizeof(contents) - 1, file) == 9);
    assert(fclose(file) == 0);
    assert(strcmp(contents, "recovered") == 0);

    remove_test_files();
}

static void test_metadata_json_contract(void)
{
    char metadata[128];
    (void)snprintf(metadata, sizeof(metadata), "%s/object.json", TEST_ROOT);
    remove_test_files();
    (void)mkdir(TEST_ROOT, 0775);

    FILE *file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":1,\"id\":\"OBJ_0001\"}", file) >= 0);
    assert(fclose(file) == 0);
    const char *reason = NULL;
    assert(object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "valid") == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0002", &reason));
    assert(strcmp(reason, "wrong_identity") == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, NULL, &reason));
    assert(strcmp(reason, "invalid_argument") == 0);
    assert(!object_recovery_fs_metadata_valid(NULL, "OBJ_0001", &reason));
    assert(strcmp(reason, "invalid_argument") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":\"one\",\"id\":\"OBJ_0001\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "invalid_schema") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":0,\"id\":\"OBJ_0001\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "unsupported_schema") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":1,\"id\":\"bad\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "invalid_identity") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":1,\"id\":\"OBJ_0002\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "wrong_identity") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("not-json", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "invalid_json") == 0);

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001", &reason));
    assert(strcmp(reason, "invalid_size") == 0);

    remove_test_files();
}

static void scan_path(char *path, size_t path_size, const char *object_id)
{
    const int written = snprintf(path, path_size, "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    assert(written >= 0 && (size_t)written < path_size);
}

static void make_directory(const char *path)
{
    errno = 0;
    if (mkdir(path, 0775) != 0) assert(errno == EEXIST);
}

static void prepare_scan_object(
    const char *object_id,
    const char *primary,
    const char *backup,
    const char *temporary
)
{
    make_directory(TEST_ROOT);
    make_directory(STORAGE_MANAGER_ROOT_PATH);
    make_directory(STORAGE_MANAGER_OBJECTS_PATH);
    char object_path[256];
    scan_path(object_path, sizeof(object_path), object_id);
    make_directory(object_path);
    if (primary != NULL) {
        char path[320];
        (void)snprintf(path, sizeof(path), "%s/object.json", object_path);
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        assert(fputs(primary, file) >= 0);
        assert(fclose(file) == 0);
    }
    if (backup != NULL) {
        char path[320];
        (void)snprintf(path, sizeof(path), "%s/object.json.bak", object_path);
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        assert(fputs(backup, file) >= 0);
        assert(fclose(file) == 0);
    }
    if (temporary != NULL) {
        char path[320];
        (void)snprintf(path, sizeof(path), "%s/object.json.tmp", object_path);
        FILE *file = fopen(path, "wb");
        assert(file != NULL);
        assert(fputs(temporary, file) >= 0);
        assert(fclose(file) == 0);
    }
}

static void remove_scan_object(const char *object_id)
{
    char object_path[256];
    char path[320];
    scan_path(object_path, sizeof(object_path), object_id);
    (void)snprintf(path, sizeof(path), "%s/object.json", object_path);
    (void)remove(path);
    (void)snprintf(path, sizeof(path), "%s/object.json.bak", object_path);
    (void)remove(path);
    (void)snprintf(path, sizeof(path), "%s/object.json.tmp", object_path);
    (void)remove(path);
    (void)rmdir(object_path);
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(TEST_ROOT);
}

static void reset_scan_fakes(void)
{
    s_active_read_result = ESP_ERR_NOT_FOUND;
    s_active_marker = NULL;
    object_recovery_test_object_leased = false;
    s_remove_result = ESP_OK;
    s_remove_tree_calls = 0U;
    s_removed_path[0] = '\0';
    s_delete_scope_result = ESP_OK;
    s_log_buffer[0] = '\0';
    s_removal_log_count = 0U;
}

static void test_scan_deletes_only_after_all_metadata_candidates_fail(void)
{
    const char *valid = "{\"schema_version\":1,\"id\":\"OBJ_0001\"}";
    bool removed = false;

    reset_scan_fakes();
    remove_scan_object("OBJ_0001");
    prepare_scan_object("OBJ_0001", "broken", valid, NULL);
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_OK);
    assert(!removed && s_remove_tree_calls == 0U);
    remove_scan_object("OBJ_0001");

    reset_scan_fakes();
    prepare_scan_object("OBJ_0001", "broken", NULL, valid);
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_OK);
    assert(!removed && s_remove_tree_calls == 0U);
    remove_scan_object("OBJ_0001");
}

static void test_active_and_leased_objects_are_never_removed(void)
{
    bool removed = false;

    reset_scan_fakes();
    prepare_scan_object("OBJ_0001", "broken", NULL, NULL);
    s_active_read_result = ESP_OK;
    s_active_marker = "OBJ_0001";
    assert(object_recovery_check_one_boot("OBJ_0001", &removed) == ESP_ERR_INVALID_STATE);
    assert(!removed && s_remove_tree_calls == 0U);

    reset_scan_fakes();
    s_active_read_result = ESP_ERR_NOT_FOUND;
    object_recovery_test_object_leased = true;
    assert(object_recovery_check_one_background("OBJ_0001", &removed) == ESP_ERR_INVALID_STATE);
    assert(!removed && s_remove_tree_calls == 0U);
    remove_scan_object("OBJ_0001");
}

static void test_unrecoverable_delete_emits_one_complete_record(void)
{
    bool removed = false;
    reset_scan_fakes();
    remove_scan_object("OBJ_0001");
    prepare_scan_object(
        "OBJ_0001",
        "not-json",
        "{\"schema_version\":0,\"id\":\"OBJ_0001\"}",
        "{\"schema_version\":1,\"id\":\"OBJ_0002\"}"
    );

    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_OK);
    assert(removed && s_remove_tree_calls == 1U);
    assert(strcmp(s_removed_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001") == 0);
    assert(s_removal_log_count == 1U);
    assert(strstr(s_log_buffer, "OBJECT_RECOVERY_REMOVED") != NULL);
    assert(strstr(s_log_buffer, "reason=unrecoverable_metadata") != NULL);
    assert(strstr(s_log_buffer, "when=runtime") != NULL);
    assert(strstr(s_log_buffer, "uptime_ms=123456") != NULL);
    assert(strstr(s_log_buffer, "boot_id=1156") != NULL);
    assert(strstr(s_log_buffer, "object=OBJ_0001") != NULL);
    assert(strstr(s_log_buffer, "sessions=2") != NULL);
    assert(strstr(s_log_buffer, "artifacts=3") != NULL);
    assert(strstr(s_log_buffer, "localization_runs=1") != NULL);
    assert(strstr(s_log_buffer, "bytes=12345") != NULL);
    assert(strstr(s_log_buffer, "primary(object.json)=invalid_json") != NULL);
    assert(strstr(s_log_buffer, "backup(object.json.bak)=unsupported_schema") != NULL);
    assert(strstr(s_log_buffer, "temporary(object.json.tmp)=wrong_identity") != NULL);

    object_recovery_deletion_record_t record = {0};
    assert(object_recovery_get_last_deletion_record(&record));
    assert(record.valid);
    assert(strcmp(record.object_id, "OBJ_0001") == 0);
    assert(strcmp(record.reason, "unrecoverable_metadata") == 0);
    assert(strcmp(record.when, "runtime") == 0);
    assert(record.uptime_ms == 123456U && record.boot_id == 1156U);
    assert(record.sessions == 2U && record.artifacts == 3U);
    assert(record.localization_runs == 1U && record.bytes == 12345U);
    assert(strstr(record.attempts, "primary(object.json)=invalid_json") != NULL);
    remove_scan_object("OBJ_0001");
}

static void test_boot_delete_records_boot_timing(void)
{
    bool removed = false;
    reset_scan_fakes();
    remove_scan_object("OBJ_0001");
    prepare_scan_object("OBJ_0001", "not-json", NULL, NULL);

    assert(object_recovery_check_one_boot("OBJ_0001", &removed) == ESP_OK);
    assert(removed && s_remove_tree_calls == 1U && s_removal_log_count == 1U);
    assert(strstr(s_log_buffer, "when=boot") != NULL);
    assert(strstr(s_log_buffer, "uptime_ms=123456") != NULL);
    assert(strstr(s_log_buffer, "boot_id=1156") != NULL);
    object_recovery_deletion_record_t record = {0};
    assert(object_recovery_get_last_deletion_record(&record));
    assert(strcmp(record.when, "boot") == 0);
    remove_scan_object("OBJ_0001");
}

static void test_delete_is_skipped_when_scope_count_fails(void)
{
    bool removed = false;
    reset_scan_fakes();
    prepare_scan_object("OBJ_0001", "not-json", NULL, NULL);
    s_delete_scope_result = ESP_FAIL;
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_FAIL);
    assert(!removed && s_remove_tree_calls == 0U && s_removal_log_count == 0U);
    remove_scan_object("OBJ_0001");
}

static void read_whole_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    const size_t count = fread(buffer, 1U, buffer_size - 1U, file);
    assert(fclose(file) == 0);
    buffer[count] = '\0';
}

static void assert_file_equals(const char *path, const char *expected)
{
    char contents[512];
    read_whole_file(path, contents, sizeof(contents));
    assert(strcmp(contents, expected) == 0);
}

static void assert_metadata_equals(const char *name, const char *expected)
{
    char path[128];
    metadata_path(path, sizeof(path), name);
    assert_file_equals(path, expected);
}

#define NEW_PRIMARY "{\"schema_version\":1,\"id\":\"OBJ_0001\",\"revision\":2}"
#define OLD_BACKUP  "{\"schema_version\":1,\"id\":\"OBJ_0001\",\"revision\":1}"

static void prepare_metadata_dir(const char *primary, const char *backup, const char *temporary)
{
    clear_faults();
    remove_test_files();
    remove_metadata("object.json.old");
    assert(mkdir(TEST_ROOT, 0775) == 0);
    if (primary != NULL) write_metadata("object.json", primary);
    if (backup != NULL) write_metadata("object.json.bak", backup);
    if (temporary != NULL) write_metadata("object.json.tmp", temporary);
}

static void cleanup_metadata_dir(void)
{
    clear_faults();
    remove_metadata("object.json.old");
    remove_test_files();
}

static void test_metadata_check_separates_unreadable_from_corrupt(void)
{
    char path[128];
    const char *reason = NULL;

    prepare_metadata_dir(NEW_PRIMARY, NULL, NULL);
    metadata_path(path, sizeof(path), "object.json");
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_VALID);

    metadata_path(path, sizeof(path), "object.json.bak");
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_MISSING);
    assert(strcmp(reason, "not_found") == 0);

    write_metadata("object.json.bak", "not-json");
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_CORRUPT);
    assert(strcmp(reason, "invalid_json") == 0);
    write_metadata("object.json.bak", "{\"schema_version\":1,\"id\":\"OBJ_0002\"}");
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_CORRUPT);
    assert(strcmp(reason, "wrong_identity") == 0);
    write_metadata("object.json.bak", "");
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_CORRUPT);
    assert(strcmp(reason, "invalid_size") == 0);

    /* Inability to judge is never reported as corruption or absence. */
    write_metadata("object.json.bak", OLD_BACKUP);
    s_fault.malloc_fail_after = 0;
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_UNREADABLE);
    assert(strcmp(reason, "no_memory") == 0);
    clear_faults();

    cJSON_stub_fail_alloc_countdown = 0;
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_UNREADABLE);
    assert(strcmp(reason, "no_memory") == 0);
    clear_faults();

    s_fault.fopen_fail_suffix = "object.json.bak";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_fs_metadata_check(path, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_UNREADABLE);
    assert(strcmp(reason, "open_error") == 0);
    clear_faults();

    assert(object_recovery_fs_metadata_check(path, NULL, &reason) == OBJECT_RECOVERY_METADATA_UNREADABLE);
    assert(object_recovery_fs_metadata_check(NULL, "OBJ_0001", &reason) == OBJECT_RECOVERY_METADATA_UNREADABLE);
    cleanup_metadata_dir();
}

static void test_unjudged_primary_never_falls_back_to_backup(void)
{
    bool valid = true;
    bool recovered = true;
    object_recovery_metadata_attempts_t attempts = {0};

    /* Out of memory while reading the healthy, newer primary. */
    prepare_metadata_dir(NEW_PRIMARY, OLD_BACKUP, NULL);
    s_fault.malloc_fail_after = 0;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_ERR_NO_MEM);
    assert(!valid && !recovered);
    assert(strcmp(attempts.primary, "no_memory") == 0);
    assert(strcmp(attempts.backup, "not_attempted") == 0);
    assert_metadata_equals("object.json", NEW_PRIMARY);
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    assert(!metadata_exists("object.json.old"));
    clear_faults();

    /* Out of memory inside the JSON parser is not "invalid JSON". */
    cJSON_stub_fail_alloc_countdown = 0;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_ERR_NO_MEM);
    assert(!valid && !recovered);
    assert(strcmp(attempts.primary, "no_memory") == 0);
    assert_metadata_equals("object.json", NEW_PRIMARY);
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    clear_faults();

    /* An SD read error on the primary is not corruption either. */
    s_fault.fopen_fail_suffix = "/object.json";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_FAIL);
    assert(!valid && !recovered);
    assert(strcmp(attempts.primary, "open_error") == 0);
    assert_metadata_equals("object.json", NEW_PRIMARY);
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    clear_faults();

    /* Once the condition clears, the same call succeeds and changes nothing. */
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(valid && !recovered);
    assert_metadata_equals("object.json", NEW_PRIMARY);
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    cleanup_metadata_dir();
}

static void test_unjudged_sidecar_stops_recovery_without_touching_files(void)
{
    bool valid = true;
    bool recovered = true;
    object_recovery_metadata_attempts_t attempts = {0};

    /* Primary is provably corrupt, but the backup cannot be read: do not skip
     * past it and promote the possibly older .tmp. */
    prepare_metadata_dir("not-json", OLD_BACKUP, NEW_PRIMARY);
    s_fault.fopen_fail_suffix = "object.json.bak";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_FAIL);
    assert(!valid && !recovered);
    assert(strcmp(attempts.primary, "invalid_json") == 0);
    assert(strcmp(attempts.backup, "open_error") == 0);
    assert(strcmp(attempts.temporary, "not_attempted") == 0);
    assert_metadata_equals("object.json", "not-json");
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    assert_metadata_equals("object.json.tmp", NEW_PRIMARY);
    clear_faults();

    /* Same for an unreadable .tmp after a corrupt primary and missing .bak. */
    remove_metadata("object.json.bak");
    s_fault.fopen_fail_suffix = "object.json.tmp";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_FAIL);
    assert(!valid && !recovered);
    assert_metadata_equals("object.json", "not-json");
    assert_metadata_equals("object.json.tmp", NEW_PRIMARY);
    cleanup_metadata_dir();
}

static void test_restore_never_loses_destination_when_rename_fails(void)
{
    char source[128];
    char destination[128];
    (void)snprintf(source, sizeof(source), "%s/source.txt", TEST_ROOT);
    (void)snprintf(destination, sizeof(destination), "%s/destination.txt", TEST_ROOT);

    remove_test_files();
    remove_metadata("destination.txt.old");
    assert(mkdir(TEST_ROOT, 0775) == 0);
    FILE *file = fopen(source, "wb");
    assert(file != NULL && fputs("recovered", file) >= 0 && fclose(file) == 0);
    file = fopen(destination, "wb");
    assert(file != NULL && fputs("stale", file) >= 0 && fclose(file) == 0);

    /* The move-aside itself fails: nothing has changed. */
    s_fault.rename_calls = 0U;
    s_fault.rename_fail_call = 1U;
    assert(object_recovery_fs_restore_file(source, destination) == ESP_FAIL);
    assert_file_equals(destination, "stale");
    assert_file_equals(source, "recovered");
    assert(!metadata_exists("destination.txt.old"));

    /* The source rename fails after the destination moved aside: it is put back. */
    s_fault.rename_calls = 0U;
    s_fault.rename_fail_call = 2U;
    assert(object_recovery_fs_restore_file(source, destination) == ESP_FAIL);
    assert_file_equals(destination, "stale");
    assert_file_equals(source, "recovered");
    assert(!metadata_exists("destination.txt.old"));
    clear_faults();

    /* Success replaces the destination and leaves no side file behind. */
    assert(object_recovery_fs_restore_file(source, destination) == ESP_OK);
    assert_file_equals(destination, "recovered");
    assert(!metadata_exists("destination.txt.old"));
    assert(!metadata_exists("source.txt"));

    /* A missing destination is a plain rename. */
    file = fopen(source, "wb");
    assert(file != NULL && fputs("again", file) >= 0 && fclose(file) == 0);
    assert(remove(destination) == 0);
    assert(object_recovery_fs_restore_file(source, destination) == ESP_OK);
    assert_file_equals(destination, "again");

    remove_metadata("destination.txt.old");
    remove_test_files();
}

static void test_failed_restore_keeps_corrupt_primary_and_backup(void)
{
    bool valid = true;
    bool recovered = true;
    object_recovery_metadata_attempts_t attempts = {0};

    prepare_metadata_dir("not-json", OLD_BACKUP, NULL);
    s_fault.rename_fail_call = 2U;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_FAIL);
    assert(!valid && !recovered);
    assert_metadata_equals("object.json", "not-json");
    assert_metadata_equals("object.json.bak", OLD_BACKUP);
    assert(!metadata_exists("object.json.old"));
    clear_faults();

    /* Retrying on the next pass completes the restore. */
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered, &attempts) == ESP_OK);
    assert(valid && recovered);
    assert_metadata_equals("object.json", OLD_BACKUP);
    assert(!metadata_exists("object.json.bak"));
    assert(!metadata_exists("object.json.old"));
    cleanup_metadata_dir();
}

static void test_scan_never_removes_object_when_metadata_cannot_be_judged(void)
{
    bool removed = false;

    /* Healthy object.json, no sidecars, out of memory while reading it. */
    reset_scan_fakes();
    remove_scan_object("OBJ_0001");
    prepare_scan_object("OBJ_0001", NEW_PRIMARY, NULL, NULL);
    s_fault.malloc_fail_after = 0;
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_ERR_NO_MEM);
    assert(!removed && s_remove_tree_calls == 0U && s_removal_log_count == 0U);
    clear_faults();

    /* Out of memory inside the JSON parser. */
    cJSON_stub_fail_alloc_countdown = 0;
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_ERR_NO_MEM);
    assert(!removed && s_remove_tree_calls == 0U);
    clear_faults();

    /* SD read error on the only candidate. */
    s_fault.fopen_fail_suffix = "/object.json";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_check_one_boot("OBJ_0001", &removed) == ESP_FAIL);
    assert(!removed && s_remove_tree_calls == 0U);
    clear_faults();

    /* A corrupt primary and an unreadable sidecar is still not "all failed". */
    remove_scan_object("OBJ_0001");
    prepare_scan_object("OBJ_0001", "not-json", OLD_BACKUP, NULL);
    s_fault.fopen_fail_suffix = "object.json.bak";
    s_fault.fopen_fail_errno = EIO;
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_FAIL);
    assert(!removed && s_remove_tree_calls == 0U);
    clear_faults();

    /* The primary file survived every attempt byte-for-byte; the next pass
     * judges it normally and still deletes only on proven corruption. */
    char path[320];
    char object_path[256];
    scan_path(object_path, sizeof(object_path), "OBJ_0001");
    (void)snprintf(path, sizeof(path), "%s/object.json", object_path);
    assert_file_equals(path, "not-json");
    remove_scan_object("OBJ_0001");

    prepare_scan_object("OBJ_0001", NEW_PRIMARY, NULL, NULL);
    (void)snprintf(path, sizeof(path), "%s/object.json", object_path);
    assert(object_recovery_check_one("OBJ_0001", &removed) == ESP_OK);
    assert(!removed && s_remove_tree_calls == 0U);
    assert_file_equals(path, NEW_PRIMARY);
    remove_scan_object("OBJ_0001");
}

int main(void)
{
    test_scan_rejects_invalid_object_ids();
    test_background_delete_guard();
    test_path_builder();
    test_unlink_and_restore();
    test_metadata_json_contract();
    test_metadata_recovery_priority();
    test_scan_deletes_only_after_all_metadata_candidates_fail();
    test_active_and_leased_objects_are_never_removed();
    test_unrecoverable_delete_emits_one_complete_record();
    test_boot_delete_records_boot_timing();
    test_delete_is_skipped_when_scope_count_fails();
    test_metadata_check_separates_unreadable_from_corrupt();
    test_unjudged_primary_never_falls_back_to_backup();
    test_unjudged_sidecar_stops_recovery_without_touching_files();
    test_restore_never_loses_destination_when_rename_fails();
    test_failed_restore_keeps_corrupt_primary_and_backup();
    test_scan_never_removes_object_when_metadata_cannot_be_judged();
    return 0;
}
