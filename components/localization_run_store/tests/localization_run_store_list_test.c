#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "localization_run_store.h"
#include "storage_manager.h"


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t read_count = fread(buffer, 1U, buffer_size - 1U, file);
    const bool truncated = fgetc(file) != EOF;
    fclose(file);
    if (truncated) {
        return ESP_ERR_INVALID_SIZE;
    }
    buffer[read_count] = '\0';
    return ESP_OK;
}


esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok ? ESP_OK : ESP_FAIL;
}


static void ensure_dir(const char *path)
{
    (void)mkdir(path, 0775);
}


static void prepare_session_directory(const char *object_id, const char *session_id)
{
    char path[256];
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)snprintf(path, sizeof(path), "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    (void)snprintf(path, sizeof(path), "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, object_id);
    ensure_dir(path);
    (void)snprintf(path, sizeof(path), "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id);
    ensure_dir(path);
}


static localization_run_record_t make_record(
    const char *object_id,
    const char *session_id,
    const char *method_name,
    uint64_t computed_at_ms,
    bool solved
)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.object_id, sizeof(record.object_id), "%s", object_id);
    snprintf(record.session_id, sizeof(record.session_id), "%s", session_id);
    record.computed_at_ms = computed_at_ms;
    record.kind = LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD;
    snprintf(record.method_name, sizeof(record.method_name), "%s", method_name);
    record.method_version = 1U;
    record.dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D;
    record.propagation_speed_mps = 340.0;
    record.solved = solved;
    if (solved) {
        record.source_position = (localization_point3d_t) { 1.0, 2.0, 3.0 };
        record.converged = true;
    }
    else {
        record.failure_class = LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN;
        snprintf(record.rejection_reason, sizeof(record.rejection_reason), "not enough picks");
    }
    return record;
}


static bool contains_method(
    const localization_run_record_t *records,
    size_t count,
    const char *method_name
)
{
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(records[i].method_name, method_name) == 0) {
            return true;
        }
    }
    return false;
}


static void cleanup_session(const char *object_id, const char *session_id)
{
    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    if (localization_run_store_list(object_id, session_id, records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count) != ESP_OK) {
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        char path[LOCALIZATION_RUN_STORE_PATH_MAX];
        if (localization_run_store_build_path(object_id, session_id, &records[i], path, sizeof(path)) == ESP_OK) {
            (void)remove(path);
        }
    }
}


static void test_lists_every_persisted_run_including_unsolved(void)
{
    prepare_session_directory("OBJ_0001", "S000001");
    cleanup_session("OBJ_0001", "S000001");

    const localization_run_record_t tdoa = make_record("OBJ_0001", "S000001", "tdoa_gauss_newton", 1000ULL, true);
    const localization_run_record_t semblance = make_record("OBJ_0001", "S000001", "semblance", 1001ULL, false);
    assert(localization_run_store_write("OBJ_0001", "S000001", &tdoa) == ESP_OK);
    assert(localization_run_store_write("OBJ_0001", "S000001", &semblance) == ESP_OK);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(localization_run_store_list("OBJ_0001", "S000001", records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count) == ESP_OK);
    assert(count == 2U);
    assert(contains_method(records, count, "tdoa_gauss_newton"));
    assert(contains_method(records, count, "semblance"));

    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(records[i].method_name, "tdoa_gauss_newton") == 0) {
            assert(records[i].solved);
            assert(records[i].source_position.x_m == 1.0);
        }
        else {
            assert(!records[i].solved);
            assert(strcmp(records[i].rejection_reason, "not enough picks") == 0);
            assert(records[i].failure_class == LOCALIZATION_FAILURE_CLASS_RECORD_AGAIN);
        }
    }

    cleanup_session("OBJ_0001", "S000001");
}


static void test_a_later_run_never_overwrites_an_earlier_one(void)
{
    prepare_session_directory("OBJ_0002", "S000001");
    cleanup_session("OBJ_0002", "S000001");

    const localization_run_record_t first = make_record("OBJ_0002", "S000001", "tdoa_gauss_newton", 1000ULL, true);
    localization_run_record_t second = make_record("OBJ_0002", "S000001", "tdoa_gauss_newton", 2000ULL, true);
    second.source_position.x_m = 9.0;
    assert(localization_run_store_write("OBJ_0002", "S000001", &first) == ESP_OK);
    assert(localization_run_store_write("OBJ_0002", "S000001", &second) == ESP_OK);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(localization_run_store_list("OBJ_0002", "S000001", records, LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count) == ESP_OK);
    /* Same method, two different computed_at_ms: both survive as separate
     * files, exactly the "versioned run, never overwritten" guarantee. */
    assert(count == 2U);

    cleanup_session("OBJ_0002", "S000001");
}


static void test_missing_session_directory_is_not_found(void)
{
    localization_run_record_t records[4];
    size_t count = 99U;
    assert(localization_run_store_list("OBJ_9999", "S000099", records, 4U, &count) == ESP_ERR_NOT_FOUND);
}


static void test_rejects_invalid_arguments(void)
{
    localization_run_record_t records[4];
    size_t count = 0U;
    assert(localization_run_store_list("OBJ_0001", "S000001", NULL, 4U, &count) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list("OBJ_0001", "S000001", records, 0U, &count) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list("OBJ_0001", "S000001", records, 4U, NULL) == ESP_ERR_INVALID_ARG);
    assert(localization_run_store_list("not-an-object", "S000001", records, 4U, &count) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_lists_every_persisted_run_including_unsolved();
    test_a_later_run_never_overwrites_an_earlier_one();
    test_missing_session_directory_is_not_found();
    test_rejects_invalid_arguments();
    puts("localization run store list tests passed");
    return 0;
}
