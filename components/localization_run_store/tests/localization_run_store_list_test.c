#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
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


static const char *s_read_failure_path;


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_read_failure_path != NULL && strcmp(path, s_read_failure_path) == 0) {
        return ESP_FAIL;
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


static localization_run_record_t make_max_v7_record(void)
{
    localization_run_record_t record = make_record(
        "OBJ_0048", "S000048", "max_record", 9007199254740991ULL, true
    );
    memset(record.method_name, 'M', sizeof(record.method_name) - 1U);
    record.method_name[sizeof(record.method_name) - 1U] = '\0';
    memset(record.propagation_model_name, 'p', sizeof(record.propagation_model_name) - 1U);
    record.propagation_model_name[sizeof(record.propagation_model_name) - 1U] = '\0';
    memset(record.material, 'm', sizeof(record.material) - 1U);
    record.material[sizeof(record.material) - 1U] = '\0';
    memset(record.scan_frame_id, 's', sizeof(record.scan_frame_id) - 1U);
    record.scan_frame_id[sizeof(record.scan_frame_id) - 1U] = '\0';
    record.scan_constraint_applied = true;
    record.occupancy_created_at_ms = 9007199254740991ULL;
    record.registration_geometry_computed_at_ms = 9007199254740991ULL;
    record.computed_at_ms = 9007199254740991ULL;
    record.geometry_computed_at_ms = 9007199254740991ULL;
    record.channel_mask_used = UINT32_MAX;
    record.channel_mask_rejected = UINT32_MAX;
    record.method_version = UINT32_MAX;
    record.propagation_speed_mps = DBL_MAX;
    record.propagation_speed_uncertainty_mps = DBL_MAX;
    record.confidence_score = DBL_MAX;
    record.terminal_status = LOCALIZATION_RUN_TERMINAL_STATUS_TIME_BUDGET_SKIPPED;
    record.session_epoch_known = true;
    record.session_epoch_timestamp_us = INT64_MIN;
    record.velocity_range_min_mps = 1.0f;
    record.velocity_range_max_mps = FLT_MAX;
    record.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_USER;
    record.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_INCONSISTENT;
    record.analysis_window_truncated = true;
    record.analysis_window_start_index = 9007199254740991ULL;
    record.analysis_window_frames = 9007199254740991ULL;
    snprintf(record.analysis_window_reason, sizeof(record.analysis_window_reason), "memory");
    record.emission_time_us = 9007199254740991LL;
    record.source_position = (localization_point3d_t) { DBL_MAX, DBL_MAX, DBL_MAX };
    record.rms_residual_us = DBL_MAX;
    record.iterations = UINT8_MAX;
    record.estimated_p_velocity_mps = DBL_MAX;
    record.estimated_s_velocity_mps = DBL_MAX;
    record.uncertainty.std_m = (localization_point3d_t) { DBL_MAX, DBL_MAX, DBL_MAX };
    record.uncertainty.credible_level = 0.999;
    record.uncertainty.credible_volume_m3 = DBL_MAX;
    record.uncertainty.credible_area_m2 = DBL_MAX;
    record.uncertainty.credible_min = (localization_point3d_t) { -DBL_MAX, -DBL_MAX, -DBL_MAX };
    record.uncertainty.credible_max = (localization_point3d_t) { DBL_MAX, DBL_MAX, DBL_MAX };
    record.uncertainty.sigma_inflation = DBL_MAX;
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
    assert(localization_run_store_list(
        "OBJ_9999", "S000099", records, 4U, &count
    ) == ESP_ERR_NOT_FOUND);

    localization_run_store_list_status_t status;
    count = 99U;
    assert(localization_run_store_list_ex(
        "OBJ_9999", "S000099", records, 4U, &count, &status
    ) == ESP_ERR_NOT_FOUND);
    assert(count == 0U && !status.partial_error && status.partial_error_count == 0U);
}


static void test_partial_error_codes_and_legacy_list_behavior(void)
{
    prepare_session_directory("OBJ_0049", "S000049");
    cleanup_session("OBJ_0049", "S000049");
    localization_run_record_t valid = make_record(
        "OBJ_0049", "S000049", "tdoa", 4900ULL, true
    );
    assert(localization_run_store_write("OBJ_0049", "S000049", &valid) == ESP_OK);

    localization_run_record_t corrupt = make_record(
        "OBJ_0049", "S000049", "broken", 4901ULL, true
    );
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(localization_run_store_build_path(
        "OBJ_0049", "S000049", &corrupt, path, sizeof(path)
    ) == ESP_OK);
    assert(storage_manager_atomic_write_text(path, "not-json") == ESP_OK);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    localization_run_store_list_status_t status;
    assert(localization_run_store_list_ex(
        "OBJ_0049", "S000049", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 1U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED) != 0U);

    count = 0U;
    assert(localization_run_store_list(
        "OBJ_0049", "S000049", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count
    ) == ESP_OK);
    assert(count == 1U);

    s_read_failure_path = path;
    count = 0U;
    assert(localization_run_store_list_ex(
        "OBJ_0049", "S000049", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 1U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_READ_FAILED) != 0U);
    s_read_failure_path = NULL;

    char non_directory_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    (void)snprintf(non_directory_path, sizeof(non_directory_path),
        "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0050", "S000050");
    char parent_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    (void)snprintf(parent_path, sizeof(parent_path),
        "%s/%s", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0050");
    ensure_dir(STORAGE_MANAGER_MOUNT_POINT);
    ensure_dir(STORAGE_MANAGER_ROOT_PATH);
    ensure_dir(STORAGE_MANAGER_OBJECTS_PATH);
    ensure_dir(parent_path);
    (void)snprintf(parent_path, sizeof(parent_path),
        "%s/%s/sessions", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0050");
    ensure_dir(parent_path);
    FILE *not_directory = fopen(non_directory_path, "wb");
    assert(not_directory != NULL);
    assert(fclose(not_directory) == 0);
    count = 0U;
    assert(localization_run_store_list_ex(
        "OBJ_0050", "S000050", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count, &status
    ) == ESP_FAIL);
    assert(count == 0U && status.partial_error && status.partial_error_count == 1U);
    assert((status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_LISTING_ENUMERATION_FAILED) != 0U);
    count = 99U;
    assert(localization_run_store_list(
        "OBJ_0050", "S000050", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count
    ) == ESP_ERR_NOT_FOUND);
    assert(count == 0U);
    (void)remove(non_directory_path);

    cleanup_session("OBJ_0049", "S000049");
    (void)remove(path);
}


static void test_max_v7_run_record_fits_store_buffer(void)
{
    prepare_session_directory("OBJ_0048", "S000048");
    cleanup_session("OBJ_0048", "S000048");
    localization_run_record_t original = make_max_v7_record();
    char *json = NULL;
    assert(localization_run_record_to_json(&original, &json) == ESP_OK);
    const size_t json_length = strlen(json);
    assert(json_length + 1U <= LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    printf("  maximal v7 run record: %u bytes, store buffer %u bytes\n",
        (unsigned)json_length, (unsigned)LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    /* Use a short recognized filename so the full maximal payload can be
     * listed under Windows MAX_PATH from the runner's nested build folder. */
    char store_path[LOCALIZATION_RUN_STORE_PATH_MAX];
    const int path_length = snprintf(
        store_path, sizeof(store_path), "%s/%s/sessions/%s/localization_run_maximal.json",
        STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0048", "S000048"
    );
    assert(path_length > 0 && (size_t)path_length < sizeof(store_path));
    (void)remove(store_path);
    assert(storage_manager_atomic_write_text(store_path, json) == ESP_OK);
    free(json);

    localization_run_record_t rejected = make_record(
        "OBJ_0048", "S000048", "max_rejected", 9007199254740990ULL, false
    );
    memset(rejected.method_name, 'M', sizeof(rejected.method_name) - 1U);
    rejected.method_name[sizeof(rejected.method_name) - 1U] = '\0';
    memset(rejected.propagation_model_name, 'p', sizeof(rejected.propagation_model_name) - 1U);
    rejected.propagation_model_name[sizeof(rejected.propagation_model_name) - 1U] = '\0';
    memset(rejected.material, 'm', sizeof(rejected.material) - 1U);
    rejected.material[sizeof(rejected.material) - 1U] = '\0';
    memset(rejected.scan_frame_id, 's', sizeof(rejected.scan_frame_id) - 1U);
    rejected.scan_frame_id[sizeof(rejected.scan_frame_id) - 1U] = '\0';
    rejected.scan_constraint_applied = true;
    rejected.occupancy_created_at_ms = 9007199254740991ULL;
    rejected.registration_geometry_computed_at_ms = 9007199254740991ULL;
    rejected.geometry_computed_at_ms = 9007199254740991ULL;
    rejected.channel_mask_used = UINT32_MAX;
    rejected.channel_mask_rejected = UINT32_MAX;
    rejected.method_version = UINT32_MAX;
    rejected.propagation_speed_mps = DBL_MAX;
    rejected.propagation_speed_uncertainty_mps = DBL_MAX;
    rejected.confidence_score = DBL_MAX;
    rejected.terminal_status = LOCALIZATION_RUN_TERMINAL_STATUS_TIME_BUDGET_SKIPPED;
    rejected.session_epoch_known = true;
    rejected.session_epoch_timestamp_us = INT64_MIN;
    rejected.velocity_range_min_mps = 1.0f;
    rejected.velocity_range_max_mps = FLT_MAX;
    rejected.velocity_range_source = LOCALIZATION_RUN_VELOCITY_SOURCE_USER;
    rejected.velocity_status = LOCALIZATION_RUN_VELOCITY_STATUS_INCONSISTENT;
    rejected.analysis_window_truncated = true;
    rejected.analysis_window_start_index = 9007199254740991ULL;
    rejected.analysis_window_frames = 9007199254740991ULL;
    snprintf(rejected.analysis_window_reason, sizeof(rejected.analysis_window_reason), "memory");
    memset(rejected.rejection_reason, 'R', sizeof(rejected.rejection_reason) - 1U);
    rejected.rejection_reason[sizeof(rejected.rejection_reason) - 1U] = '\0';
    assert(localization_run_record_to_json(&rejected, &json) == ESP_OK);
    const size_t rejected_json_length = strlen(json);
    assert(rejected_json_length + 1U <= LOCALIZATION_RUN_STORE_JSON_BUFFER_SIZE);
    printf("  maximal rejected v7 run record: %u bytes\n", (unsigned)rejected_json_length);
    free(json);

    localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    localization_run_store_list_status_t status;
    assert(localization_run_store_list_ex(
        "OBJ_0048", "S000048", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &count, &status
    ) == ESP_OK);
    assert(count == 1U && !status.partial_error);
    assert(strcmp(records[0].method_name, original.method_name) == 0);
    assert(records[0].computed_at_ms == original.computed_at_ms);
    assert(records[0].terminal_status == original.terminal_status);
    assert(records[0].session_epoch_known);
    assert(records[0].session_epoch_timestamp_us == original.session_epoch_timestamp_us);
    assert(records[0].uncertainty.credible_level == original.uncertainty.credible_level);
    assert(records[0].analysis_window_frames == original.analysis_window_frames);
    (void)remove(store_path);
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
    test_max_v7_run_record_fits_store_buffer();
    test_lists_every_persisted_run_including_unsolved();
    test_a_later_run_never_overwrites_an_earlier_one();
    test_partial_error_codes_and_legacy_list_behavior();
    test_missing_session_directory_is_not_found();
    test_rejects_invalid_arguments();
    puts("localization run store list tests passed");
    return 0;
}
