#include <assert.h>
#include <errno.h>
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

#include "object_recovery_fs.h"
#include "object_recovery_internal.h"
#include "storage_manager.h"

static const char *TEST_ROOT = "object_recovery_contract_tmp";

static esp_err_t s_active_read_result = ESP_ERR_NOT_FOUND;
static const char *s_active_marker = NULL;

unsigned esp_log_timestamp(void)
{
    return 0U;
}

esp_err_t storage_manager_remove_tree(const char *path)
{
    (void)path;
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

    remove_test_files();
    assert(mkdir(TEST_ROOT, 0775) == 0);
    write_metadata("object.json", "{\"schema_version\":1,\"id\":\"OBJ_0001\"}");
    write_metadata("object.json.tmp", "stale");
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered) == ESP_OK);
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
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered) == ESP_OK);
    assert(valid);
    assert(recovered);
    assert(metadata_exists("object.json"));
    assert(!metadata_exists("object.json.bak"));
    assert(!metadata_exists("object.json.tmp"));

    remove_metadata("object.json");
    write_metadata("object.json.tmp", "{\"schema_version\":1,\"id\":\"OBJ_0001\"}");
    valid = false;
    recovered = false;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered) == ESP_OK);
    assert(valid);
    assert(recovered);
    assert(metadata_exists("object.json"));
    assert(!metadata_exists("object.json.tmp"));
    assert(!metadata_exists("object.json.bak"));

    remove_metadata("object.json");
    write_metadata("object.json", "not-json");
    valid = true;
    recovered = true;
    assert(object_recovery_fs_prepare_metadata(TEST_ROOT, "OBJ_0001", &valid, &recovered) == ESP_OK);
    assert(!valid);
    assert(!recovered);
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
    assert(object_recovery_fs_metadata_valid(metadata, "OBJ_0001"));
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0002"));
    assert(!object_recovery_fs_metadata_valid(metadata, NULL));
    assert(!object_recovery_fs_metadata_valid(NULL, "OBJ_0001"));

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":0,\"id\":\"OBJ_0001\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001"));

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("{\"schema_version\":1,\"id\":\"OBJ_0002\"}", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001"));

    file = fopen(metadata, "wb");
    assert(file != NULL);
    assert(fputs("not-json", file) >= 0);
    assert(fclose(file) == 0);
    assert(!object_recovery_fs_metadata_valid(metadata, "OBJ_0001"));

    remove_test_files();
}

int main(void)
{
    test_scan_rejects_invalid_object_ids();
    test_background_delete_guard();
    test_path_builder();
    test_unlink_and_restore();
    test_metadata_json_contract();
    test_metadata_recovery_priority();
    return 0;
}
