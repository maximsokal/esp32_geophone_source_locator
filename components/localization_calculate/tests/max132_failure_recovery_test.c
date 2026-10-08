#include "localization_run_store.h"

#define main max132_unused_localization_calculate_run_test_main
#define localization_run_store_write max132_unused_localization_run_store_write
#define localization_run_store_write_picks max132_unused_localization_run_store_write_picks
#include "localization_calculate_run_test.c"
#undef localization_run_store_write_picks
#undef localization_run_store_write
#undef main

#include <stdint.h>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ext_test_support.h"
#include "localization_bayes.h"
#include "localization_calculate_policy.h"
#include "localization_event_run_record.h"
#include "localization_eikonal.h"
#include "localization_psafuvs.h"
#include "localization_psp.h"
#include "localization_robust.h"

#ifndef MAX132_FAILURE_SWEEP_FIRST
#define MAX132_FAILURE_SWEEP_FIRST 1U
#endif
#ifndef MAX132_FAILURE_SWEEP_LAST
#define MAX132_FAILURE_SWEEP_LAST 0U
#endif
#ifndef MAX132_FAILURE_SWEEP_EXPECTED_POINTS
#define MAX132_FAILURE_SWEEP_EXPECTED_POINTS 0U
#endif

void max132_fault_allocator_start(size_t fail_at);
void max132_fault_allocator_start_method(const char *method_name);
void max132_fault_allocator_stop(void);
void max132_fault_allocator_set_method(const char *method_name);
size_t max132_fault_allocator_attempts(void);
size_t max132_fault_allocator_live_blocks(void);
size_t max132_fault_allocator_live_bytes(void);
bool max132_fault_allocator_hit(void);
size_t max132_fault_allocator_method_attempts(const char *method_name);

static size_t s_pick_write_failures_remaining;
static size_t s_pick_write_failures;

#ifdef _WIN32
#define MAX132_TEST_PROCESS_ID _getpid
#else
#define MAX132_TEST_PROCESS_ID getpid
#endif

static void make_directories(const char *path)
{
    char prefix[LOCALIZATION_RUN_STORE_PATH_MAX];
    const size_t length = strlen(path);
    assert(length > 0U && length < sizeof(prefix));
    memcpy(prefix, path, length + 1U);
    for (size_t i = 0U; i <= length; ++i) {
        if (prefix[i] == '/' || prefix[i] == '\\' || prefix[i] == '\0') {
            const char saved = prefix[i];
            prefix[i] = '\0';
            if (prefix[0] != '\0') {
#ifdef _WIN32
                (void)_mkdir(prefix);
#else
                (void)mkdir(prefix, 0775);
#endif
            }
            prefix[i] = saved;
        }
    }
}

static void ensure_parent_directory(const char *file_path)
{
    char parent[LOCALIZATION_RUN_STORE_PATH_MAX];
    const size_t length = strlen(file_path);
    assert(length > 0U && length < sizeof(parent));
    memcpy(parent, file_path, length + 1U);
    char *separator = strrchr(parent, '/');
    char *backslash = strrchr(parent, '\\');
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    assert(separator != NULL);
    *separator = '\0';
    make_directories(parent);
}

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
    (void)fclose(file);
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
    if (s_pick_write_failures_remaining > 0U &&
        strstr(path, "localization_picks_") != NULL) {
        --s_pick_write_failures_remaining;
        ++s_pick_write_failures;
        return ESP_FAIL;
    }
    ensure_parent_directory(path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const bool ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok ? ESP_OK : ESP_FAIL;
}

static void allocator_progress(
    const char *method_name,
    bool completed,
    size_t methods_completed,
    size_t methods_total,
    void *context
)
{
    (void)methods_completed;
    (void)methods_total;
    (void)context;
    max132_fault_allocator_set_method(completed ? NULL : method_name);
}

static void setup_fixture(void)
{
    reset_fakes();
    build_fixture(4U, &s_source, TEST_SPEED_MPS, TEST_EMISSION_S, 1.0f, 0.02f, true);
}

static void format_session_id(size_t value, char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE])
{
    const int written = snprintf(session_id, LOCALIZATION_RUN_SESSION_ID_SIZE, "S%06zu", value);
    assert(written == 7 && written < (int)LOCALIZATION_RUN_SESSION_ID_SIZE);
}

static esp_err_t read_session(
    const char *session_id,
    localization_run_record_t *records,
    size_t *record_count
)
{
    return localization_run_store_list("OBJ_0001", session_id, records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, record_count);
}

static const localization_run_record_t *find_saved(
    const localization_run_record_t *records,
    size_t count,
    const char *method_name
)
{
    for (size_t i = 0U; i < count; ++i) {
        if (strcmp(records[i].method_name, method_name) == 0) {
            return &records[i];
        }
    }
    return NULL;
}

static bool saved_records_are_complete_and_explained(
    const localization_run_record_t *expected,
    size_t expected_count,
    const localization_run_record_t *saved,
    size_t saved_count
)
{
    if (expected_count == 0U || saved_count != expected_count) {
        return false;
    }

    for (size_t i = 0U; i < expected_count; ++i) {
        if (expected[i].method_name[0] == '\0') {
            return false;
        }
        const localization_run_record_t *record =
            find_saved(saved, saved_count, expected[i].method_name);
        if (record == NULL || record->schema_version != LOCALIZATION_RUN_SCHEMA_VERSION ||
            record->kind != expected[i].kind) {
            return false;
        }
        if (!record->solved &&
            (record->rejection_reason[0] == '\0' ||
             record->failure_class == LOCALIZATION_FAILURE_CLASS_NONE)) {
            return false;
        }
    }

    for (size_t i = 0U; i < saved_count; ++i) {
        if (saved[i].method_name[0] == '\0') {
            return false;
        }
        for (size_t j = i + 1U; j < saved_count; ++j) {
            if (strcmp(saved[i].method_name, saved[j].method_name) == 0) {
                return false;
            }
        }
    }
    return true;
}


static bool is_defined_allocation_fault_error(esp_err_t err)
{
    return err == ESP_FAIL || err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_TIMEOUT ||
        err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC ||
        err == ESP_ERR_INVALID_VERSION;
}

static void run_clean_and_check(
    const char *session_id,
    uint64_t computed_at_ms,
    bool track_method_allocations,
    localization_run_record_t *records_out,
    size_t *record_count_out
)
{
    setup_fixture();
    localization_calculate_config_t config = default_test_config();
    if (track_method_allocations) {
        config.progress = allocator_progress;
    }
    localization_calculate_summary_t summary;
    const size_t live_before = max132_fault_allocator_live_blocks();
    const size_t bytes_before = max132_fault_allocator_live_bytes();
    max132_fault_allocator_start(0U);
    const esp_err_t err = localization_calculate_run(
        "OBJ_0001", session_id, &config, computed_at_ms, &summary
    );
    assert(err == ESP_OK);
    assert(!max132_fault_allocator_hit());
    max132_fault_allocator_stop();
    assert(max132_fault_allocator_live_blocks() == live_before);
    assert(max132_fault_allocator_live_bytes() == bytes_before);
    assert(localization_test_psram_model_allocated_bytes() == 0U);
    assert(summary.methods_persisted == summary.methods_total);
    assert(read_session(session_id, records_out, record_count_out) == ESP_OK);
    assert(*record_count_out == summary.methods_total);
}

static void test_every_calculation_allocation_failure_recovers(
    size_t sweep_first,
    size_t sweep_last_requested
)
{
    static localization_run_record_t baseline[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t baseline_count = 0U;
    const char *baseline_session = "S010000";
    setup_fixture();
    localization_calculate_config_t config = default_test_config();
    config.progress = allocator_progress;
    localization_calculate_summary_t summary;
    const size_t live_before = max132_fault_allocator_live_blocks();
    const size_t bytes_before = max132_fault_allocator_live_bytes();
    max132_fault_allocator_start(0U);
    const esp_err_t clean_err = localization_calculate_run(
        "OBJ_0001", baseline_session, &config, 1000000U, &summary
    );
    const size_t allocation_points = max132_fault_allocator_attempts();
    assert(clean_err == ESP_OK && allocation_points > 0U);
#if MAX132_FAILURE_SWEEP_EXPECTED_POINTS > 0U
    assert(allocation_points == MAX132_FAILURE_SWEEP_EXPECTED_POINTS);
#endif
    FILE *allocation_count_file = fopen("allocation-count.txt", "wb");
    assert(allocation_count_file != NULL);
    assert(fprintf(allocation_count_file, "%zu\n", allocation_points) > 0);
    assert(fclose(allocation_count_file) == 0);
    max132_fault_allocator_stop();
    assert(max132_fault_allocator_live_blocks() == live_before);
    assert(max132_fault_allocator_live_bytes() == bytes_before);
    assert(localization_test_psram_model_allocated_bytes() == 0U);
    assert(read_session(baseline_session, baseline, &baseline_count) == ESP_OK);
    assert(baseline_count == summary.methods_total);

    const size_t sweep_last = sweep_last_requested == 0U || sweep_last_requested > allocation_points
        ? allocation_points
        : sweep_last_requested;
    assert(sweep_first > 0U && sweep_first <= sweep_last && sweep_last <= allocation_points);
    for (size_t fail_at = sweep_first; fail_at <= sweep_last; ++fail_at) {
        char failed_session[LOCALIZATION_RUN_SESSION_ID_SIZE];
        char recovered_session[LOCALIZATION_RUN_SESSION_ID_SIZE];
        format_session_id(200000U + fail_at * 2U, failed_session);
        format_session_id(200001U + fail_at * 2U, recovered_session);
        setup_fixture();
        localization_calculate_config_t failed_config = default_test_config();
        localization_calculate_summary_t failed_summary;
        localization_run_record_t failed_records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
        size_t failed_count = 0U;
        const size_t live_before_failure = max132_fault_allocator_live_blocks();
        const size_t bytes_before_failure = max132_fault_allocator_live_bytes();
        max132_fault_allocator_start(fail_at);
        const esp_err_t failed_err = localization_calculate_run(
            "OBJ_0001", failed_session, &failed_config, 2000000U + fail_at, &failed_summary
        );
        const size_t attempts = max132_fault_allocator_attempts();
        const bool injected = max132_fault_allocator_hit();
        max132_fault_allocator_stop();
        assert(injected && attempts >= fail_at);
        assert(max132_fault_allocator_live_blocks() == live_before_failure);
        assert(max132_fault_allocator_live_bytes() == bytes_before_failure);
        assert(localization_test_psram_model_allocated_bytes() == 0U);
        assert(failed_summary.methods_persisted <= failed_summary.methods_total);

        const esp_err_t list_err = read_session(failed_session, failed_records, &failed_count);
        if (list_err == ESP_ERR_NOT_FOUND && failed_summary.methods_persisted == 0U) {
            failed_count = 0U;
        } else {
            assert(list_err == ESP_OK);
            if (failed_count != failed_summary.methods_persisted) {
                fprintf(stderr,
                    "I1 N=%zu returned %d with unreadable persistence: listed=%zu summary=%zu\n",
                    fail_at, (int)failed_err, failed_count, failed_summary.methods_persisted);
            }
            assert(failed_count == failed_summary.methods_persisted);
        }
        for (size_t i = 0U; i < failed_count; ++i) {
            assert(failed_records[i].method_name[0] != '\0');
            if (!failed_records[i].solved) {
                assert(failed_records[i].rejection_reason[0] != '\0');
                assert(failed_records[i].failure_class != LOCALIZATION_FAILURE_CLASS_NONE);
            }
        }

        if (failed_err == ESP_OK) {
            if (!saved_records_are_complete_and_explained(
                    baseline, baseline_count, failed_records, failed_count)) {
                fprintf(stderr,
                    "I1 N=%zu returned ESP_OK with incomplete or unexplained records: "
                    "listed=%zu persisted=%zu total=%zu\n",
                    fail_at, failed_count, failed_summary.methods_persisted,
                    failed_summary.methods_total);
            }
            assert(saved_records_are_complete_and_explained(
                baseline, baseline_count, failed_records, failed_count));
            assert(failed_summary.methods_persisted == failed_summary.methods_total);
            for (size_t i = 0U; i < failed_count; ++i) {
                assert(failed_records[i].computed_at_ms == 2000000U + fail_at);
            }
            localization_pick_record_t failed_picks[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
            size_t failed_pick_count = 0U;
            assert(localization_run_store_list_picks("OBJ_0001", failed_session, failed_picks,
                LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &failed_pick_count) == ESP_OK);
            assert(failed_pick_count == 1U);
        } else {
            assert(is_defined_allocation_fault_error(failed_err));
        }

        static localization_run_record_t recovered[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
        size_t recovered_count = 0U;
        run_clean_and_check(recovered_session, 3000000U + fail_at, false,
            recovered, &recovered_count);
        const localization_run_record_t *joint = find_saved(recovered, recovered_count, "joint_fusion");
        assert(joint != NULL);
        localization_pick_record_t picks[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
        size_t pick_count = 0U;
        assert(localization_run_store_list_picks("OBJ_0001", recovered_session, picks,
            LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &pick_count) == ESP_OK);
        assert(pick_count == 1U);
    }

    free_fixture_frames();
    printf("  allocation sweep: N=%zu..%zu of %zu points; success requires every explained record, "
        "otherwise the run must return an error\n",
        sweep_first, sweep_last, allocation_points);
}
static void isolate_test_store(void)
{
    char run_directory[64];
    const int written = snprintf(run_directory, sizeof(run_directory),
        "run_%lu", (unsigned long)MAX132_TEST_PROCESS_ID());
    assert(written > 0 && written < (int)sizeof(run_directory));
    make_directories(run_directory);
#ifdef _WIN32
    assert(_chdir(run_directory) == 0);
#else
    assert(chdir(run_directory) == 0);
#endif
}

static bool joint_matches_saved_individuals(
    const localization_run_record_t *records,
    size_t record_count,
    const localization_run_record_t *joint
)
{
    localization_comparison_method_result_t methods[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t method_count = 0U;
    for (size_t i = 0U; i < record_count; ++i) {
        if (&records[i] == joint || records[i].kind != LOCALIZATION_RUN_KIND_INDIVIDUAL_METHOD) {
            continue;
        }
        localization_calculate_run_record_to_comparison_method(&records[i], &methods[method_count++]);
    }
    assert(method_count > 0U);
    localization_comparison_result_t fused;
    assert(localization_comparison_fuse(methods, method_count, &fused) == ESP_OK);
    localization_calculate_run_context_t context = {
        .object_id = joint->object_id,
        .session_id = joint->session_id,
        .computed_at_ms = joint->computed_at_ms,
        .geometry_computed_at_ms = joint->geometry_computed_at_ms,
        .dimensionality = joint->dimensionality,
        .propagation_model_name = joint->propagation_model_name,
        .material = joint->material,
        .propagation_speed_mps = joint->propagation_speed_mps,
        .propagation_speed_uncertainty_mps = joint->propagation_speed_uncertainty_mps,
        .velocity_range = NULL,
    };
    localization_run_record_t expected;
    localization_calculate_joint_to_run_record(
        &context, &fused, joint->channel_mask_used, joint->channel_mask_rejected,
        joint->method_version, &expected
    );
    if (expected.solved != joint->solved) return false;
    if (!joint->solved) return joint->rejection_reason[0] != '\0';
    return fabs(expected.source_position.x_m - joint->source_position.x_m) < 1.0e-9 &&
        fabs(expected.source_position.y_m - joint->source_position.y_m) < 1.0e-9 &&
        fabs(expected.source_position.z_m - joint->source_position.z_m) < 1.0e-9;
}

static void test_method_local_allocation_failures_preserve_other_records(void)
{
    static const char *const methods[] = {
        "tdoa_gauss_newton", "traveltime_inversion", "semblance", "energy_baseline",
        "tdcas", "psafuvs", "suea", "xcorr_tdoa", "robust_tdoa", "psp_joint",
        "bayesian", "eikonal_fmm"
    };
    localization_run_record_t baseline[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t baseline_count = 0U;
    run_clean_and_check("S020000", 4000000U, true, baseline, &baseline_count);
    size_t injected_backend_count = 0U;
    size_t joint_failed_method_mismatch_count = 0U;
    static const char *const expected_joint_mismatch_methods[] = {
        "semblance", "suea", "xcorr_tdoa", "robust_tdoa", "bayesian"
    };
    bool joint_mismatch_seen[sizeof(expected_joint_mismatch_methods) /
        sizeof(expected_joint_mismatch_methods[0])] = { false };
    for (size_t method_index = 0U; method_index < sizeof(methods) / sizeof(methods[0]); ++method_index) {
        const char *method = methods[method_index];
        if (max132_fault_allocator_method_attempts(method) == 0U) {
            continue;
        }
        ++injected_backend_count;
        char session_id[LOCALIZATION_RUN_SESSION_ID_SIZE];
        format_session_id(210000U + method_index, session_id);
        setup_fixture();
        localization_calculate_config_t config = default_test_config();
        config.progress = allocator_progress;
        localization_calculate_summary_t summary;
        max132_fault_allocator_start_method(method);
        const esp_err_t err = localization_calculate_run(
            "OBJ_0001", session_id, &config, 5000000U + method_index, &summary
        );
        const bool injected = max132_fault_allocator_hit();
        max132_fault_allocator_stop();
        assert(injected);
        assert(err == ESP_OK);

        localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
        size_t count = 0U;
        assert(read_session(session_id, records, &count) == ESP_OK);
        assert(count == baseline_count);
        const localization_run_record_t *failed = find_saved(records, count, method);
        assert(failed != NULL && !failed->solved && failed->rejection_reason[0] != '\0');
        for (size_t i = 0U; i < baseline_count; ++i) {
            const localization_run_record_t *persisted = find_saved(records, count, baseline[i].method_name);
            assert(persisted != NULL);
            if (strcmp(baseline[i].method_name, method) != 0) {
                assert(persisted->computed_at_ms == 5000000U + method_index);
            }
        }
        const localization_run_record_t *joint = find_saved(records, count, "joint_fusion");
        assert(joint != NULL);
        if (!joint_matches_saved_individuals(records, count, joint)) {
            /* KNOWN_DEFECT, build/max132_survey.md:112: a failed method's unsaved solver
             * candidate still influences joint_fusion for the five method
             * names above. Minimal repro: inject an allocation failure into
             * one listed backend, then fuse the readable records; the saved
             * joint differs. */
            fprintf(stderr, "KNOWN_DEFECT build/max132_survey.md:112 joint mismatch after %s allocation failure\n", method);
            bool expected_mismatch = false;
            for (size_t i = 0U; i < sizeof(expected_joint_mismatch_methods) /
                sizeof(expected_joint_mismatch_methods[0]); ++i) {
                if (strcmp(method, expected_joint_mismatch_methods[i]) == 0) {
                    assert(!joint_mismatch_seen[i]);
                    joint_mismatch_seen[i] = true;
                    expected_mismatch = true;
                    break;
                }
            }
            assert(expected_mismatch);
            ++joint_failed_method_mismatch_count;
        }
        assert(summary.methods_persisted == summary.methods_total);
        assert(localization_test_psram_model_allocated_bytes() == 0U);
    }
    assert(injected_backend_count >= 2U);
    assert(joint_failed_method_mismatch_count ==
        sizeof(expected_joint_mismatch_methods) / sizeof(expected_joint_mismatch_methods[0]));
    for (size_t i = 0U; i < sizeof(joint_mismatch_seen) / sizeof(joint_mismatch_seen[0]); ++i) {
        assert(joint_mismatch_seen[i]);
    }
    free_fixture_frames();
    printf("  method-local allocation failures: %zu allocating backends preserved readable records; joint audited\n",
        injected_backend_count);
    puts("  KNOWN_DEFECT build/max132_survey.md:112: five failed backends still change persisted joint fusion");
}

static void write_raw_file(const char *path, const char *contents)
{
    assert(storage_manager_atomic_write_text(path, contents) == ESP_OK);
}

static void write_bad_run_file(const char *session_id, const char *method, uint64_t computed_at_ms,
    const char *contents)
{
    localization_run_record_t record;
    memset(&record, 0, sizeof(record));
    snprintf(record.object_id, sizeof(record.object_id), "%s", "OBJ_0001");
    snprintf(record.session_id, sizeof(record.session_id), "%s", session_id);
    snprintf(record.method_name, sizeof(record.method_name), "%s", method);
    record.computed_at_ms = computed_at_ms;
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(localization_run_store_build_path("OBJ_0001", session_id, &record,
        path, sizeof(path)) == ESP_OK);
    write_raw_file(path, contents);
}

static void write_bad_pick_file(const char *session_id, uint64_t computed_at_ms,
    const char *contents)
{
    char path[LOCALIZATION_RUN_STORE_PATH_MAX];
    assert(localization_run_store_build_picks_path("OBJ_0001", session_id, computed_at_ms,
        path, sizeof(path)) == ESP_OK);
    write_raw_file(path, contents);
}

static void test_corrupt_saved_records_are_skipped(void)
{
    /* KNOWN_DEFECT, build/max132_survey.md:125-126: the run-history HTTP handler also turns
     * an injected picks-list error into "picks":[]; its real production
     * handler assertion runs in this manifest's max132_failure_api_known_defect target. */
    static const char *const corrupt[] = {
        "{\"schema_version\":", "", "not-json", "{\"schema_version\":999}"
    };
    static const char *const bad_methods[] = {
        "bad_truncated", "bad_empty", "bad_garbage", "bad_schema"
    };
    static localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;

    run_clean_and_check("S070001", 7000000U, false, records, &count);
    for (size_t i = 0U; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        write_bad_run_file("S070001", bad_methods[i], 7100000U + i, corrupt[i]);
        write_bad_pick_file("S070001", 7200000U + i, corrupt[i]);
    }
    assert(read_session("S070001", records, &count) == ESP_OK);
    assert(count == 13U);
    assert(find_saved(records, count, "tdoa_gauss_newton") != NULL);
    localization_pick_record_t picks[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t pick_count = 0U;
    assert(localization_run_store_list_picks("OBJ_0001", "S070001", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &pick_count) == ESP_OK);
    assert(pick_count == 1U);

    localization_run_store_list_status_t run_status;
    size_t extended_run_count = 0U;
    assert(localization_run_store_list_ex("OBJ_0001", "S070001", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &extended_run_count,
        &run_status) == ESP_OK);
    assert(extended_run_count == 13U);
    assert(run_status.partial_error && run_status.partial_error_count == 4U);
    assert((run_status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED) != 0U);

    localization_run_store_list_status_t pick_status;
    size_t extended_pick_count = 0U;
    assert(localization_run_store_list_picks_ex("OBJ_0001", "S070001", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &extended_pick_count,
        &pick_status) == ESP_OK);
    assert(extended_pick_count == 1U);
    assert(pick_status.partial_error && pick_status.partial_error_count == 4U);
    assert((pick_status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED) != 0U);

    for (size_t i = 0U; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
        write_bad_run_file("S070002", bad_methods[i], 7300000U + i, corrupt[i]);
        write_bad_pick_file("S070002", 7400000U + i, corrupt[i]);
    }
    size_t corrupt_run_count = 99U;
    assert(read_session("S070002", records, &corrupt_run_count) == ESP_OK);
    assert(corrupt_run_count == 0U);
    size_t corrupt_pick_count = 99U;
    assert(localization_run_store_list_picks("OBJ_0001", "S070002", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &corrupt_pick_count) == ESP_OK);
    assert(corrupt_pick_count == 0U);

    assert(localization_run_store_list_ex("OBJ_0001", "S070002", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &corrupt_run_count,
        &run_status) == ESP_OK);
    assert(corrupt_run_count == 0U);
    assert(run_status.partial_error && run_status.partial_error_count == 4U);
    assert((run_status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_RUN_RECORD_PARSE_FAILED) != 0U);
    assert(localization_run_store_list_picks_ex("OBJ_0001", "S070002", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &corrupt_pick_count,
        &pick_status) == ESP_OK);
    assert(corrupt_pick_count == 0U);
    assert(pick_status.partial_error && pick_status.partial_error_count == 4U);
    assert((pick_status.partial_error_codes &
        LOCALIZATION_RUN_STORE_PARTIAL_ERROR_PICKS_RECORD_PARSE_FAILED) != 0U);

    const char *empty_session = "S070003";
    char empty_directory[LOCALIZATION_RUN_STORE_PATH_MAX];
    (void)snprintf(empty_directory, sizeof(empty_directory),
        "%s/%s/sessions/%s", STORAGE_MANAGER_OBJECTS_PATH, "OBJ_0001", empty_session);
    make_directories(empty_directory);
    size_t empty_run_count = 99U;
    assert(read_session(empty_session, records, &empty_run_count) == ESP_OK);
    assert(empty_run_count == corrupt_run_count);
    size_t empty_pick_count = 99U;
    assert(localization_run_store_list_picks("OBJ_0001", empty_session, picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &empty_pick_count) == ESP_OK);
    assert(empty_pick_count == corrupt_pick_count);

    assert(localization_run_store_list_ex("OBJ_0001", empty_session, records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &empty_run_count,
        &run_status) == ESP_OK);
    assert(empty_run_count == 0U && !run_status.partial_error);
    assert(localization_run_store_list_picks_ex("OBJ_0001", empty_session, picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &empty_pick_count,
        &pick_status) == ESP_OK);
    assert(empty_pick_count == 0U && !pick_status.partial_error);

    size_t missing_count = 99U;
    assert(localization_run_store_list_ex("OBJ_0001", "S070004", records,
        LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION, &missing_count,
        &run_status) == ESP_ERR_NOT_FOUND);
    assert(missing_count == 0U && !run_status.partial_error);
    missing_count = 99U;
    assert(localization_run_store_list_picks_ex("OBJ_0001", "S070004", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &missing_count,
        &pick_status) == ESP_ERR_NOT_FOUND);
    assert(missing_count == 0U && !pick_status.partial_error);

    free_fixture_frames();
    puts("  corrupt run/pick records: valid entries preserved with partial-error codes; empty/missing distinguished");
}

static void test_tdoa_iteration_cap_is_persisted_as_unconverged(void)
{
    /* KNOWN_DEFECT, build/max132_survey.md:108: hitting the one-step cap leaves the saved
     * candidate marked solved with no rejection reason. The convergence flag
     * is false, but downstream readers can mistake the candidate for a final
     * solution. Minimal repro: start far from the source with max_iterations=1. */
    localization_event_channel_input_t channels[4];
    const localization_point3d_t far_guess = { 50.0, 50.0, 50.0 };
    const uint32_t sample_rate_hz = 50000U;
    const int64_t emission_time_us = 1000000LL;
    for (size_t i = 0U; i < 4U; ++i) {
        const double dx = s_source.x_m - s_pod_positions[i].x_m;
        const double dy = s_source.y_m - s_pod_positions[i].y_m;
        const double dz = s_source.z_m - s_pod_positions[i].z_m;
        const double travel_us = sqrt(dx * dx + dy * dy + dz * dz) /
            (double)TEST_SPEED_MPS * 1000000.0;
        channels[i] = (localization_event_channel_input_t){
            .pick = {
                .status = GEOPHONE_PICK_ACCEPTED,
                .arrival_sample_index = (size_t)llround(
                    ((double)emission_time_us + travel_us) * sample_rate_hz / 1000000.0
                ),
            },
            .sample_rate_hz = sample_rate_hz,
            .sensor_position = s_pod_positions[i],
        };
    }
    localization_config_t config = localization_default_config();
    config.propagation_speed_mps = TEST_SPEED_MPS;
    config.max_iterations = 1U;
    config.convergence_m = 1.0e-12;
    localization_event_result_t event;
    assert(localization_event_solve(channels, 4U, &config, &far_guess, &event) == ESP_OK);
    assert(event.status == LOCALIZATION_EVENT_SOLVED);
    assert(event.solver_result.iterations == config.max_iterations);
    assert(!event.solver_result.converged);

    localization_run_record_t record;
    assert(localization_event_result_to_run_record(
        &event, "OBJ_0001", "S090001", 9000001U, 9000000U, 1U,
        "test-model", "custom", TEST_SPEED_MPS, 20.0, 60.0, &record
    ) == ESP_OK);
    assert(record.solved && record.iterations == 1U && !record.converged);
    assert(record.rejection_reason[0] == '\0');
    assert(localization_run_store_write("OBJ_0001", "S090001", &record) == ESP_OK);
    localization_run_record_t persisted[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(read_session("S090001", persisted, &count) == ESP_OK && count == 1U);
    assert(persisted[0].solved && persisted[0].iterations == 1U && !persisted[0].converged);
    puts("  TDOA cap: 1 iteration persisted with converged=false");
}

static localization_calculate_run_context_t cap_test_context(
    const char *session_id,
    uint64_t computed_at_ms
)
{
    return (localization_calculate_run_context_t){
        .object_id = "OBJ_0001",
        .session_id = session_id,
        .computed_at_ms = computed_at_ms,
        .geometry_computed_at_ms = computed_at_ms - 1U,
        .dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
        .propagation_model_name = "test-model",
        .material = "custom",
        .propagation_speed_mps = 300.0,
        .propagation_speed_uncertainty_mps = 10.0,
        .velocity_range = NULL,
    };
}

static void persist_and_read_cap_result(
    const char *session_id,
    const localization_run_record_t *record
)
{
    assert(localization_run_store_write("OBJ_0001", session_id, record) == ESP_OK);
    localization_run_record_t persisted[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(read_session(session_id, persisted, &count) == ESP_OK && count == 1U);
    assert(persisted[0].solved);
    assert(strcmp(persisted[0].method_name, record->method_name) == 0);
}

static void test_psp_iteration_cap_is_not_persisted(void)
{
    const localization_point3d_t source = { 1.7, 1.4, -1.2 };
    localization_psp_observation_t observations[6];
    uint32_t state = 31U;
    for (size_t i = 0U; i < 6U; ++i) {
        const double distance = ext_test_distance(source, EXT_TEST_SENSORS_3D[i]);
        const double noise_p = ext_test_normal(&state) * 80.0;
        const double noise_s = ext_test_normal(&state) * 100.0;
        observations[i] = (localization_psp_observation_t){
            .sensor_position = EXT_TEST_SENSORS_3D[i],
            .p_arrival_time_us = (int64_t)llround((0.02 + distance / 300.0) * 1.0e6 + noise_p),
            .s_valid = true,
            .s_arrival_time_us = (int64_t)llround((0.02 + distance / 150.0) * 1.0e6 + noise_s),
            .s_minus_p_sigma_us = 150.0,
        };
    }
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_psp_config_t config = {
        .timing_uncertainty_us = 20.0,
        .p_velocity_mps = 300.0,
        .p_velocity_sigma_mps = 10.0,
        .max_iterations = 1U,
    };
    localization_psp_result_t result;
    assert(localization_psp_solve(observations, 6U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSP_SOLVED);
    assert(result.iterations == config.max_iterations && !result.converged);

    /* KNOWN_DEFECT, build/max132_survey.md:108: the PSP converter drops its cap/convergence
     * status. Minimal repro: noisy six-pick fit with max_iterations=1. */
    const localization_calculate_run_context_t context = cap_test_context("S090003", 9000003U);
    localization_run_record_t record;
    localization_calculate_psp_to_run_record(&context, &result, NULL,
        LOCALIZATION_FAILURE_CLASS_INTERNAL, 0x3FU, 0U, 1U, 20.0, &record);
    assert(record.solved && record.iterations == 0U && !record.converged);
    assert(record.rejection_reason[0] == '\0');
    persist_and_read_cap_result("S090003", &record);
    puts("  KNOWN_DEFECT build/max132_survey.md:108: PSP one-iteration cap is omitted from its saved run record");
}

static void test_psafuvs_iteration_cap_is_persisted_without_rejection(void)
{
    const localization_point3d_t source = { 2.2, 1.4, -1.1 };
    localization_psafuvs_observation_t observations[6];
    for (size_t i = 0U; i < 6U; ++i) {
        const double distance = ext_test_distance(source, EXT_TEST_SENSORS_3D[i]);
        const double perturbation = (i % 2U == 0U) ? 90.0 : -70.0;
        observations[i] = (localization_psafuvs_observation_t){
            .sensor_position = EXT_TEST_SENSORS_3D[i],
            .p_arrival_time_us = (int64_t)llround((1.0 + distance / 410.0) * 1.0e6 + perturbation),
            .s_arrival_time_us = (int64_t)llround((1.0 + distance / 230.0) * 1.0e6 - perturbation),
        };
    }
    localization_psafuvs_config_t config = localization_psafuvs_default_config();
    config.max_iterations = 1U;
    config.convergence_m = 1.0e-12;
    localization_psafuvs_result_t result;
    assert(localization_psafuvs_solve(observations, 6U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_PSAFUVS_SOLVED);
    assert(result.iterations == config.max_iterations && !result.converged);

    /* KNOWN_DEFECT, build/max132_survey.md:108: a cap-hit PSAFUVS candidate is still saved
     * as solved, with no rejection reason. Minimal repro: perturbed six-pick
     * fit with max_iterations=1. */
    const localization_calculate_run_context_t context = cap_test_context("S090004", 9000004U);
    localization_run_record_t record;
    localization_calculate_psafuvs_to_run_record(&context, &result, NULL,
        LOCALIZATION_FAILURE_CLASS_INTERNAL, 0x3FU, 0U, 1U, 20.0, &record, NULL);
    assert(record.solved && record.iterations == 1U && !record.converged);
    assert(record.rejection_reason[0] == '\0');
    persist_and_read_cap_result("S090004", &record);
    puts("  KNOWN_DEFECT build/max132_survey.md:108: PSAFUVS cap-hit candidate is persisted as solved");
}

static void test_bayesian_cell_budget_limit_has_no_persisted_status(void)
{
    const localization_point3d_t source = { 1.7, 1.4, -1.2 };
    localization_bayes_pick_t picks[6];
    for (size_t i = 0U; i < 6U; ++i) {
        picks[i].sensor_position = EXT_TEST_SENSORS_3D[i];
        picks[i].arrival_time_us = (int64_t)llround(
            (0.01 + ext_test_distance(source, picks[i].sensor_position) / 300.0) * 1.0e6
        );
        picks[i].weight = 1.0;
    }
    localization_bayes_config_t config = {
        .velocity_mps = 300.0,
        .velocity_sigma_mps = 0.0,
        .timing_uncertainty_us = 20.0,
        .max_cells = 64U,
        .refine_divisor = 2U,
    };
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_bayes_result_t result;
    assert(localization_bayes_locate(picks, 6U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_BAYES_SOLVED);
    assert(result.cells_evaluated > 0U);
    assert(result.timing_uncertainty_used_us > config.timing_uncertainty_us);

    /* KNOWN_DEFECT, build/max132_survey.md:108: a cell-budget-limited posterior has no
     * persisted budget/convergence status. Minimal repro: six picks with
     * max_cells=64, then serialize the Bayes method record. */
    const localization_calculate_run_context_t context = cap_test_context("S090005", 9000005U);
    localization_run_record_t record;
    localization_calculate_bayes_to_run_record(&context, &result, "bayesian", NULL,
        LOCALIZATION_FAILURE_CLASS_INTERNAL, false, 0x3FU, 0U, 1U, 20.0, &record);
    assert(record.solved && record.rejection_reason[0] == '\0');
    assert(record.iterations == 0U && !record.converged);
    persist_and_read_cap_result("S090005", &record);
    puts("  KNOWN_DEFECT build/max132_survey.md:108: Bayesian cell-budget status is omitted from its saved run record");
}

static void test_eikonal_node_cap_resolution_is_not_persisted(void)
{
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_eikonal_model_t model;
    assert(localization_eikonal_model_init(&model, &bounds.min_corner, &bounds.max_corner,
        bounds.step_m, 64U, 300.0) == ESP_OK);
    assert(localization_eikonal_node_count(&model) <= 64U);
    assert(model.cell_m > bounds.step_m);

    localization_eikonal_fields_t fields;
    assert(localization_eikonal_fields_compute(&model, EXT_TEST_SENSORS_3D, 6U, &fields) == ESP_OK);
    localization_eikonal_pick_t picks[6];
    const localization_point3d_t source = { 1.7, 1.4, -1.2 };
    for (size_t i = 0U; i < 6U; ++i) {
        const double travel_s = localization_eikonal_time_at(&model, fields.fields[i], &source);
        assert(isfinite(travel_s) && travel_s > 0.0);
        picks[i] = (localization_eikonal_pick_t){
            .arrival_time_us = (int64_t)llround((0.01 + travel_s) * 1.0e6),
            .weight = 1.0,
        };
    }
    const localization_eikonal_locate_config_t config = {
        .timing_uncertainty_us = 20.0,
        .velocity_scale_sigma = 0.0,
        .refine_divisor = 2U,
    };
    localization_eikonal_result_t result;
    assert(localization_eikonal_locate(&model, &fields, picks, 6U, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_EIKONAL_SOLVED && result.candidates_evaluated > 0U);

    /* KNOWN_DEFECT, build/max132_survey.md:108: the max_nodes cap coarsens the FMM grid,
     * but the saved result omits the actual cell spacing/budget status.
     * Minimal repro: six picks, requested cell=0.1 m, max_nodes=64. */
    const localization_calculate_run_context_t context = cap_test_context("S090006", 9000006U);
    localization_run_record_t record;
    localization_calculate_eikonal_to_run_record(&context, &result, "eikonal_fmm", NULL,
        LOCALIZATION_FAILURE_CLASS_INTERNAL, false, 0x3FU, 0U, 1U, 20.0, &record);
    assert(record.solved && record.rejection_reason[0] == '\0');
    persist_and_read_cap_result("S090006", &record);
    localization_eikonal_fields_free(&fields);
    localization_eikonal_model_free(&model);
    puts("  KNOWN_DEFECT build/max132_survey.md:108: coarse FMM grid resolution is omitted from its saved run record");
}

static void test_robust_iteration_cap_status_is_not_persisted(void)
{
    /* KNOWN_DEFECT, build/max132_survey.md:108: robust records use the default zero/false
     * TDOA-only iteration fields even after their configured IRLS cap is hit.
     * Minimal repro: solve six picks with max_iterations=1, then persist. */
    const localization_point3d_t source = { 1.7, 1.4, -1.2 };
    uint32_t random_state = 11U;
    localization_observation_t observations[6];
    ext_test_observations(EXT_TEST_SENSORS_3D, 6U, source, 0.010, 300.0, 20.0,
        &random_state, observations);
    const localization_extended_bounds_t bounds = ext_test_bounds_3d();
    localization_robust_config_t config = localization_robust_default_config();
    config.propagation_speed_mps = 300.0;
    config.timing_uncertainty_us = 20.0;
    config.consensus = false;
    config.max_iterations = 1U;
    localization_robust_result_t result;
    assert(localization_robust_solve(observations, 6U, &bounds, &config, &result) == ESP_OK);
    assert(result.status == LOCALIZATION_ROBUST_SOLVED);
    assert(result.irls_iterations == config.max_iterations);

    localization_calculate_run_context_t context = {
        .object_id = "OBJ_0001",
        .session_id = "S090002",
        .computed_at_ms = 9000002U,
        .geometry_computed_at_ms = 9000000U,
        .dimensionality = LOCALIZATION_DIMENSIONALITY_CAPABLE_3D,
        .propagation_model_name = "test-model",
        .material = "custom",
        .propagation_speed_mps = 300.0,
        .propagation_speed_uncertainty_mps = 20.0,
        .velocity_range = NULL,
    };
    localization_run_record_t record;
    localization_calculate_robust_to_run_record(
        &context, &result, 0x3FU, 0U, 1U, 20.0, &record
    );
    assert(record.solved);
    assert(record.iterations == 0U && !record.converged && record.rejection_reason[0] == '\0');
    assert(localization_run_store_write("OBJ_0001", "S090002", &record) == ESP_OK);
    localization_run_record_t persisted[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    size_t count = 0U;
    assert(read_session("S090002", persisted, &count) == ESP_OK && count == 1U);
    assert(persisted[0].solved && persisted[0].iterations == 0U && !persisted[0].converged);
    puts("  KNOWN_DEFECT build/max132_survey.md:108: robust cap is omitted from its saved run record");
}

static void test_pick_write_failure_retries_and_reports(void)
{
    static localization_run_record_t records[LOCALIZATION_RUN_STORE_MAX_RUNS_PER_SESSION];
    setup_fixture();
    s_pick_write_failures_remaining = 1U;
    s_pick_write_failures = 0U;
    localization_calculate_config_t config = default_test_config();
    localization_calculate_summary_t summary;
    const esp_err_t err = localization_calculate_run(
        "OBJ_0001", "S080001", &config, 8000000U, &summary
    );
    assert(err == ESP_OK);
    assert(s_pick_write_failures == 1U && s_pick_write_failures_remaining == 0U);
    size_t count = 0U;
    assert(read_session("S080001", records, &count) == ESP_OK);
    assert(count == summary.methods_total);
    localization_pick_record_t picks[LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION];
    size_t pick_count = 99U;
    assert(localization_run_store_list_picks("OBJ_0001", "S080001", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &pick_count) == ESP_OK);
    assert(pick_count == 1U);

    setup_fixture();
    s_pick_write_failures_remaining = 2U;
    s_pick_write_failures = 0U;
    localization_calculate_summary_t failed_summary;
    const esp_err_t failed_err = localization_calculate_run(
        "OBJ_0001", "S080002", &config, 8000001U, &failed_summary
    );
    assert(failed_err != ESP_OK);
    assert(s_pick_write_failures == 2U && s_pick_write_failures_remaining == 0U);
    count = 0U;
    assert(read_session("S080002", records, &count) == ESP_OK);
    assert(count == failed_summary.methods_total);
    pick_count = 99U;
    assert(localization_run_store_list_picks("OBJ_0001", "S080002", picks,
        LOCALIZATION_RUN_STORE_MAX_PICK_RECORDS_PER_SESSION, &pick_count) == ESP_OK);
    assert(pick_count == 0U);
    free_fixture_frames();
    puts("  pick writes retry once; persistent failure is returned after run records persist");
}

int main(void)
{
    isolate_test_store();
    test_every_calculation_allocation_failure_recovers(
        MAX132_FAILURE_SWEEP_FIRST,
        MAX132_FAILURE_SWEEP_LAST
    );
    test_method_local_allocation_failures_preserve_other_records();
    test_corrupt_saved_records_are_skipped();
    test_pick_write_failure_retries_and_reports();
    test_tdoa_iteration_cap_is_persisted_as_unconverged();
    test_robust_iteration_cap_status_is_not_persisted();
    test_psp_iteration_cap_is_not_persisted();
    test_psafuvs_iteration_cap_is_persisted_without_rejection();
    test_bayesian_cell_budget_limit_has_no_persisted_status();
    test_eikonal_node_cap_resolution_is_not_persisted();
    localization_test_psram_model_reset();
    assert(max132_fault_allocator_live_blocks() == 0U);
    puts("MAX-132 failure/recovery integration passed");
    return 0;
}

