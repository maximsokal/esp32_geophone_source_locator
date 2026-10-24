#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main web_status_json_builder_test_main
#include "web_status_json_builder_test.c"
#undef main

#include "cjson_counting_alloc.h"

#define WEB_STATUS_BUDGET_ITERATIONS 5000U
#define WEB_STATUS_BUDGET_SUCCESS_WINDOW_ITERATIONS 500U
#define WEB_STATUS_BUDGET_EXACT_LIMIT_ITERATIONS 50U
#define WEB_STATUS_BUDGET_FAILURE_ITERATIONS 20U
#define WEB_STATUS_BUDGET_ALLOCS_CEILING 1310U
#define WEB_STATUS_BUDGET_PEAK_BYTES_CEILING 68000U
#define WEB_STATUS_BUDGET_JSON_BYTES_CEILING 12288U
#define WEB_STATUS_BUDGET_EXPECTED_NODE_COUNT 619U

typedef struct {
    const char *label;
    void (*setup)(void);
} budget_scenario_t;

typedef struct {
    cjson_counting_alloc_stats_t baseline;
    cjson_counting_alloc_stats_t per_build;
    char *ref_json;
    uint64_t allocs_per_build;
    uint64_t requested_bytes_total;
    size_t peak_bytes;
    size_t peak_small_bytes;
    size_t peak_large_bytes;
    size_t json_bytes;
    size_t node_count;
} budget_result_t;

typedef struct {
    uint64_t points;
    uint64_t nomem;
    uint64_t ok_despite_failure;
    uint64_t other_error;
    uint64_t leaks;
} budget_sweep_stats_t;

static void budget_setup_full(void)
{
    reset_fakes();
    fill_status();
    g_mdns_ready = true;
    g_mdns_retry_count = 2U;
    g_mdns_next_retry_delay_ms = 5000U;
}

static void budget_setup_idle(void)
{
    reset_fakes();
}

static bool budget_matches_baseline(
    const cjson_counting_alloc_stats_t *actual,
    const cjson_counting_alloc_stats_t *baseline
)
{
    return actual->live_blocks == baseline->live_blocks &&
        actual->live_bytes == baseline->live_bytes &&
        actual->live_small_bytes == baseline->live_small_bytes &&
        actual->live_large_bytes == baseline->live_large_bytes;
}

static void budget_assert_baseline(
    const cjson_counting_alloc_stats_t *actual,
    const cjson_counting_alloc_stats_t *baseline
)
{
    assert(budget_matches_baseline(actual, baseline));
}

static size_t budget_count_nodes(const cJSON *item)
{
    size_t count = 0U;
    for (const cJSON *current = item; current != NULL; current = current->next) {
        ++count;
        count += budget_count_nodes(current->child);
    }
    return count;
}

static void budget_measure_scenario(
    const budget_scenario_t *scenario,
    budget_result_t *result
)
{
    assert(scenario != NULL);
    assert(scenario->setup != NULL);
    assert(result != NULL);

    scenario->setup();
    char *warm_json = NULL;
    const esp_err_t warm_err = web_status_json_build(&warm_json);
    assert(warm_err == ESP_OK);
    assert(warm_json != NULL);
    result->json_bytes = strlen(warm_json);
    assert(result->json_bytes > 0U);
    assert(result->json_bytes < SIZE_MAX);

    cJSON *parsed = cJSON_Parse(warm_json);
    assert(parsed != NULL);
    result->node_count = budget_count_nodes(parsed);
    cJSON_Delete(parsed);

    result->ref_json = (char *)malloc(result->json_bytes + 1U);
    assert(result->ref_json != NULL);
    memcpy(result->ref_json, warm_json, result->json_bytes + 1U);
    web_status_json_free(warm_json);

    cjson_counting_alloc_get_stats(&result->baseline);
    const cjson_counting_alloc_stats_t baseline = result->baseline;

    uint64_t min_allocs = UINT64_MAX;
    uint64_t max_allocs = 0U;
    uint64_t min_requested_bytes = UINT64_MAX;
    uint64_t max_requested_bytes = 0U;
    size_t min_peak = SIZE_MAX;
    size_t max_peak = 0U;
    size_t min_peak_small = SIZE_MAX;
    size_t max_peak_small = 0U;
    size_t min_peak_large = SIZE_MAX;
    size_t max_peak_large = 0U;
    bool have_iteration = false;

    for (unsigned int iteration = 0U;
         iteration < WEB_STATUS_BUDGET_ITERATIONS;
         ++iteration) {
        cjson_counting_alloc_reset_counters();
        char *json = NULL;
        const esp_err_t err = web_status_json_build(&json);
        assert(err == ESP_OK);
        assert(json != NULL);

        cjson_counting_alloc_stats_t after_build;
        cjson_counting_alloc_get_stats(&after_build);
        const size_t current_json_bytes = strlen(json);
        assert(current_json_bytes == result->json_bytes);
        assert(after_build.live_blocks == baseline.live_blocks + 1U);
        assert(current_json_bytes < SIZE_MAX);
        assert(baseline.live_bytes <= SIZE_MAX - current_json_bytes - 1U);
        assert(after_build.live_bytes >=
            baseline.live_bytes + current_json_bytes + 1U);
        assert(strcmp(json, result->ref_json) == 0);

        web_status_json_free(json);
        json = NULL;

        cjson_counting_alloc_stats_t after_free;
        cjson_counting_alloc_get_stats(&after_free);
        budget_assert_baseline(&after_free, &baseline);
        assert(after_free.blocks_allocated == after_free.blocks_freed);
        const uint64_t allocs = after_free.malloc_calls +
            after_free.calloc_calls + after_free.realloc_calls;
        assert(allocs > 0U);
        assert(after_free.peak_live_bytes >= baseline.live_bytes);
        assert(after_free.peak_small_bytes >= baseline.live_small_bytes);
        assert(after_free.peak_large_bytes >= baseline.live_large_bytes);

        const size_t peak = after_free.peak_live_bytes - baseline.live_bytes;
        const size_t peak_small =
            after_free.peak_small_bytes - baseline.live_small_bytes;
        const size_t peak_large =
            after_free.peak_large_bytes - baseline.live_large_bytes;
        const uint64_t requested = after_free.requested_bytes_total;
        if (!have_iteration) {
            min_allocs = allocs;
            max_allocs = allocs;
            min_requested_bytes = requested;
            max_requested_bytes = requested;
            min_peak = peak;
            max_peak = peak;
            min_peak_small = peak_small;
            max_peak_small = peak_small;
            min_peak_large = peak_large;
            max_peak_large = peak_large;
            have_iteration = true;
        } else {
            if (allocs < min_allocs) min_allocs = allocs;
            if (allocs > max_allocs) max_allocs = allocs;
            if (requested < min_requested_bytes) min_requested_bytes = requested;
            if (requested > max_requested_bytes) max_requested_bytes = requested;
            if (peak < min_peak) min_peak = peak;
            if (peak > max_peak) max_peak = peak;
            if (peak_small < min_peak_small) min_peak_small = peak_small;
            if (peak_small > max_peak_small) max_peak_small = peak_small;
            if (peak_large < min_peak_large) min_peak_large = peak_large;
            if (peak_large > max_peak_large) max_peak_large = peak_large;
        }
        result->per_build = after_free;
    }

    assert(have_iteration);
    assert(min_allocs == max_allocs);
    assert(min_requested_bytes == max_requested_bytes);
    assert(min_peak == max_peak);
    assert(min_peak_small == max_peak_small);
    assert(min_peak_large == max_peak_large);
    result->allocs_per_build = min_allocs;
    result->requested_bytes_total = min_requested_bytes;
    result->peak_bytes = min_peak;
    result->peak_small_bytes = min_peak_small;
    result->peak_large_bytes = min_peak_large;
}

static void budget_print_report(
    const budget_scenario_t *scenario,
    const budget_result_t *result
)
{
    const char *summary_prefix = NULL;
    const char *detail_prefix = NULL;
    const char *histogram_prefix = NULL;
    if (strcmp(scenario->label, "full") == 0) {
        summary_prefix = "STATUS_BUILD_BUDGET";
        detail_prefix = "STATUS_BUILD_BUDGET_DETAIL";
        histogram_prefix = "STATUS_BUILD_BUDGET_HISTOGRAM";
    } else {
        assert(strcmp(scenario->label, "idle") == 0);
        summary_prefix = "STATUS_BUILD_BUDGET_IDLE";
        detail_prefix = "STATUS_BUILD_BUDGET_IDLE_DETAIL";
        histogram_prefix = "STATUS_BUILD_BUDGET_IDLE_HISTOGRAM";
    }

    assert(result->allocs_per_build > 0U);
    const uint64_t average_alloc_size =
        result->requested_bytes_total / result->allocs_per_build;
    printf(
        "%s allocs_per_build=%llu peak_bytes=%llu avg_alloc_size=%llu json_bytes=%llu node_count=%llu\n",
        summary_prefix,
        (unsigned long long)result->allocs_per_build,
        (unsigned long long)result->peak_bytes,
        (unsigned long long)average_alloc_size,
        (unsigned long long)result->json_bytes,
        (unsigned long long)result->node_count
    );
    fflush(stdout);

    printf(
        "%s scenario=%s iterations=%llu mallocs=%llu callocs_nodes=%llu reallocs=%llu frees=%llu node_size=%llu peak_small_bytes=%llu peak_large_bytes=%llu largest_request=%llu requested_bytes_total=%llu\n",
        detail_prefix,
        scenario->label,
        (unsigned long long)WEB_STATUS_BUDGET_ITERATIONS,
        (unsigned long long)result->per_build.malloc_calls,
        (unsigned long long)result->per_build.calloc_calls,
        (unsigned long long)result->per_build.realloc_calls,
        (unsigned long long)result->per_build.free_calls,
        (unsigned long long)cjson_counting_alloc_node_size(),
        (unsigned long long)result->peak_small_bytes,
        (unsigned long long)result->peak_large_bytes,
        (unsigned long long)result->per_build.largest_request,
        (unsigned long long)result->requested_bytes_total
    );
    fflush(stdout);

    printf(
        "%s scenario=%s le16=%llu le32=%llu le64=%llu le128=%llu le256=%llu le512=%llu le1024=%llu le2048=%llu le4096=%llu le8192=%llu gt8192=%llu\n",
        histogram_prefix,
        scenario->label,
        (unsigned long long)result->per_build.histogram[0],
        (unsigned long long)result->per_build.histogram[1],
        (unsigned long long)result->per_build.histogram[2],
        (unsigned long long)result->per_build.histogram[3],
        (unsigned long long)result->per_build.histogram[4],
        (unsigned long long)result->per_build.histogram[5],
        (unsigned long long)result->per_build.histogram[6],
        (unsigned long long)result->per_build.histogram[7],
        (unsigned long long)result->per_build.histogram[8],
        (unsigned long long)result->per_build.histogram[9],
        (unsigned long long)result->per_build.histogram[10]
    );
    fflush(stdout);
}

static void budget_run_limited_success(
    const budget_result_t *result,
    size_t limit,
    unsigned int iterations
)
{
    cjson_counting_alloc_set_limit(limit);
    for (unsigned int iteration = 0U; iteration < iterations; ++iteration) {
        cjson_counting_alloc_reset_counters();
        char *json = NULL;
        const esp_err_t err = web_status_json_build(&json);
        assert(err == ESP_OK);
        assert(json != NULL);
        assert(strcmp(json, result->ref_json) == 0);
        web_status_json_free(json);

        cjson_counting_alloc_stats_t after_free;
        cjson_counting_alloc_get_stats(&after_free);
        budget_assert_baseline(&after_free, &result->baseline);
        assert(after_free.failures_injected == 0U);
    }
}

static void budget_run_limited_failure(
    const budget_result_t *result,
    size_t limit,
    unsigned int iterations
)
{
    cjson_counting_alloc_set_limit(limit);
    for (unsigned int iteration = 0U; iteration < iterations; ++iteration) {
        cjson_counting_alloc_reset_counters();
        char *json = NULL;
        const esp_err_t err = web_status_json_build(&json);
        const bool json_was_null = json == NULL;
        if (json != NULL) web_status_json_free(json);

        cjson_counting_alloc_stats_t after_free;
        cjson_counting_alloc_get_stats(&after_free);
        assert(err == ESP_ERR_NO_MEM);
        assert(json_was_null);
        assert(after_free.failures_injected >= 1U);
        budget_assert_baseline(&after_free, &result->baseline);
    }
}

static void budget_fail_sweep(
    uint64_t allocation_index,
    esp_err_t result,
    bool json_was_nonnull,
    const char *reason,
    const cjson_counting_alloc_stats_t *actual,
    const cjson_counting_alloc_stats_t *baseline
)
{
    fprintf(
        stderr,
        "STATUS_BUILD_FAIL_SWEEP_VIOLATION k=%llu err=%d json_nonnull=%u reason=%s live_blocks=%llu/%llu live_bytes=%llu/%llu\n",
        (unsigned long long)allocation_index,
        (int)result,
        json_was_nonnull ? 1U : 0U,
        reason,
        (unsigned long long)actual->live_blocks,
        (unsigned long long)baseline->live_blocks,
        (unsigned long long)actual->live_bytes,
        (unsigned long long)baseline->live_bytes
    );
    exit(EXIT_FAILURE);
}

static void budget_run_fail_sweep(const budget_result_t *result)
{
    budget_sweep_stats_t sweep = {0};
    cjson_counting_alloc_set_limit(0U);

    for (uint64_t allocation_index = 1U;
         allocation_index <= result->allocs_per_build;
         ++allocation_index) {
        ++sweep.points;
        cjson_counting_alloc_reset_counters();
        cjson_counting_alloc_set_fail_index(allocation_index);
        char *json = NULL;
        const esp_err_t err = web_status_json_build(&json);
        const bool json_was_nonnull = json != NULL;

        if (err == ESP_ERR_NO_MEM && !json_was_nonnull) {
            ++sweep.nomem;
        } else if (err == ESP_OK && json_was_nonnull) {
            ++sweep.ok_despite_failure;
        } else {
            ++sweep.other_error;
        }

        if (json != NULL) {
            web_status_json_free(json);
            json = NULL;
        }

        cjson_counting_alloc_stats_t after_cleanup;
        cjson_counting_alloc_get_stats(&after_cleanup);
        if (err != ESP_ERR_NO_MEM || json_was_nonnull) {
            budget_fail_sweep(
                allocation_index,
                err,
                json_was_nonnull,
                "expected ESP_ERR_NO_MEM and NULL json_out",
                &after_cleanup,
                &result->baseline
            );
        }
        if (after_cleanup.failures_injected != 1U) {
            budget_fail_sweep(
                allocation_index,
                err,
                json_was_nonnull,
                "expected exactly one injected allocation failure",
                &after_cleanup,
                &result->baseline
            );
        }
        const bool no_leak = budget_matches_baseline(
            &after_cleanup,
            &result->baseline
        );
        if (!no_leak) {
            ++sweep.leaks;
            budget_fail_sweep(
                allocation_index,
                err,
                json_was_nonnull,
                "outstanding allocations after cleanup",
                &after_cleanup,
                &result->baseline
            );
        }
    }

    cjson_counting_alloc_set_fail_index(0U);
    printf(
        "STATUS_BUILD_FAIL_SWEEP points=%llu nomem=%llu ok_despite_failure=%llu other_error=%llu leaks=%llu\n",
        (unsigned long long)sweep.points,
        (unsigned long long)sweep.nomem,
        (unsigned long long)sweep.ok_despite_failure,
        (unsigned long long)sweep.other_error,
        (unsigned long long)sweep.leaks
    );
    fflush(stdout);
    if (
        sweep.points != result->allocs_per_build ||
        sweep.nomem != sweep.points ||
        sweep.ok_despite_failure != 0U ||
        sweep.other_error != 0U ||
        sweep.leaks != 0U
    ) {
        fprintf(
            stderr,
            "STATUS_BUILD_FAIL_SWEEP_ASSERT points=%llu expected=%llu nomem=%llu ok_despite_failure=%llu other_error=%llu leaks=%llu\n",
            (unsigned long long)sweep.points,
            (unsigned long long)result->allocs_per_build,
            (unsigned long long)sweep.nomem,
            (unsigned long long)sweep.ok_despite_failure,
            (unsigned long long)sweep.other_error,
            (unsigned long long)sweep.leaks
        );
        exit(EXIT_FAILURE);
    }
}

static void budget_assert_regression_ceilings(
    const budget_scenario_t *scenario,
    const budget_result_t *result
)
{
    /* Full-scenario measurement with the per-pod sensor_health object (object
     * plus six booleans = 7 nodes x 4 pods = +28 nodes over the previous 591):
     * 1281 allocs, 66401 peak bytes, 10467 JSON bytes, and 619 cJSON nodes.
     * The allocation ceiling keeps ~2% headroom over that measurement. */
    if (result->allocs_per_build > WEB_STATUS_BUDGET_ALLOCS_CEILING) {
        fprintf(stderr, "%s allocation ceiling exceeded: %llu > %u\n",
                scenario->label,
                (unsigned long long)result->allocs_per_build,
                WEB_STATUS_BUDGET_ALLOCS_CEILING);
        exit(EXIT_FAILURE);
    }
    if (result->peak_bytes > WEB_STATUS_BUDGET_PEAK_BYTES_CEILING) {
        fprintf(stderr, "%s peak byte ceiling exceeded: %llu > %u\n",
                scenario->label,
                (unsigned long long)result->peak_bytes,
                WEB_STATUS_BUDGET_PEAK_BYTES_CEILING);
        exit(EXIT_FAILURE);
    }
    if (result->json_bytes > WEB_STATUS_BUDGET_JSON_BYTES_CEILING) {
        fprintf(stderr, "%s JSON byte ceiling exceeded: %llu > %u\n",
                scenario->label,
                (unsigned long long)result->json_bytes,
                WEB_STATUS_BUDGET_JSON_BYTES_CEILING);
        exit(EXIT_FAILURE);
    }
    if (
        strcmp(scenario->label, "full") == 0 &&
        result->node_count != WEB_STATUS_BUDGET_EXPECTED_NODE_COUNT
    ) {
        fprintf(stderr, "%s cJSON node count changed: %llu != %u\n",
                scenario->label,
                (unsigned long long)result->node_count,
                WEB_STATUS_BUDGET_EXPECTED_NODE_COUNT);
        exit(EXIT_FAILURE);
    }
}

static void budget_run_constrained_full(const budget_result_t *result)
{
    const size_t baseline_bytes = result->baseline.live_bytes;
    const size_t peak_bytes = result->peak_bytes;
    assert(peak_bytes > 0U);
    assert(peak_bytes <= SIZE_MAX - baseline_bytes);
    const size_t baseline_plus_peak = baseline_bytes + peak_bytes;
    assert(baseline_plus_peak <= SIZE_MAX - 4096U);

    cjson_counting_alloc_set_fail_index(0U);
    budget_run_limited_success(
        result,
        baseline_plus_peak + 4096U,
        WEB_STATUS_BUDGET_SUCCESS_WINDOW_ITERATIONS
    );
    budget_run_limited_success(
        result,
        baseline_plus_peak,
        WEB_STATUS_BUDGET_EXACT_LIMIT_ITERATIONS
    );

    const size_t three_quarters_peak =
        (peak_bytes / 4U) * 3U + ((peak_bytes % 4U) * 3U) / 4U;
    const size_t failure_limits[5] = {
        baseline_plus_peak - 1U,
        baseline_bytes + three_quarters_peak,
        baseline_bytes + peak_bytes / 2U,
        baseline_bytes + peak_bytes / 4U,
        baseline_bytes
    };
    for (size_t index = 0U; index < 5U; ++index) {
        /* At B == 0, a limit of zero means unlimited, so skip the B-only case. */
        if (index == 4U && baseline_bytes == 0U) continue;
        budget_run_limited_failure(
            result,
            failure_limits[index],
            WEB_STATUS_BUDGET_FAILURE_ITERATIONS
        );
    }

    cjson_counting_alloc_set_limit(0U);
    cjson_counting_alloc_reset_counters();
    char *json = NULL;
    const esp_err_t recovery_err = web_status_json_build(&json);
    assert(recovery_err == ESP_OK);
    assert(json != NULL);
    assert(strcmp(json, result->ref_json) == 0);
    web_status_json_free(json);
    cjson_counting_alloc_stats_t after_recovery;
    cjson_counting_alloc_get_stats(&after_recovery);
    budget_assert_baseline(&after_recovery, &result->baseline);

    budget_run_fail_sweep(result);
}

static void budget_release_result(budget_result_t *result)
{
    free(result->ref_json);
    result->ref_json = NULL;
}

int main(void)
{
    const int builder_test_result = web_status_json_builder_test_main();
    assert(builder_test_result == 0);

    const budget_scenario_t full_scenario = {"full", budget_setup_full};
    budget_result_t full_result = {0};
    budget_measure_scenario(&full_scenario, &full_result);

    const budget_scenario_t idle_scenario = {"idle", budget_setup_idle};
    budget_result_t idle_result = {0};
    budget_measure_scenario(&idle_scenario, &idle_result);

    budget_print_report(&full_scenario, &full_result);
    budget_print_report(&idle_scenario, &idle_result);

    budget_assert_regression_ceilings(&full_scenario, &full_result);
    budget_assert_regression_ceilings(&idle_scenario, &idle_result);

    full_scenario.setup();
    budget_run_constrained_full(&full_result);
    budget_release_result(&full_result);
    budget_release_result(&idle_result);

    puts("web_status_json_alloc_budget_test: PASS");
    return 0;
}
