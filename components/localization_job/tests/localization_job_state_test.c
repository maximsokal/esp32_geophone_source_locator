#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization_job.h"

/*
 * The background "Calculate source" job's state machine: one job at a
 * time, the deadline handed to the run, progress and the measured
 * timings / stack high-water mark published as status JSON.
 */

static localization_calculate_config_t config(void)
{
    localization_calculate_config_t c;
    memset(&c, 0, sizeof(c));
    c.propagation_speed_mps = 300.0;
    c.timing_uncertainty_us = 20.0;
    c.velocity_range.bounds = (localization_velocity_bounds_t) {
        .v_min_mps = 250.0f,
        .v_max_mps = 350.0f,
        .vp_vs_min = 1.5f,
        .vp_vs_max = 2.0f,
        .set = true,
    };
    c.velocity_range.source = LOCALIZATION_VELOCITY_RANGE_SOURCE_USER;
    return c;
}


static void test_one_job_at_a_time_with_its_deadline(void)
{
    localization_job_state_t state;
    localization_job_state_init(&state, LOCALIZATION_JOB_STACK_BYTES);
    localization_job_status_t status;
    localization_job_state_snapshot(&state, 0, &status);
    assert(status.state == LOCALIZATION_JOB_IDLE && status.job_id == 0U);

    const localization_calculate_config_t c = config();
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", &c, 0U, 777U) == ESP_OK);
    assert(state.status.state == LOCALIZATION_JOB_QUEUED && state.status.job_id == 1U);
    assert(state.status.budget_ms == LOCALIZATION_JOB_DEFAULT_BUDGET_MS);
    assert(state.status.computed_at_ms == 777U);
    assert(state.status.progress_percent == 0U);
    /* A second submit while one is pending is refused, not queued. */
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000002", &c, 0U, 778U) == ESP_ERR_INVALID_STATE);

    localization_calculate_config_t run;
    assert(localization_job_state_start(&state, 5000000, &run));
    assert(state.status.progress_percent == 0U);
    assert(run.deadline_us == 5000000 + (int64_t)LOCALIZATION_JOB_DEFAULT_BUDGET_MS * 1000);
    assert(run.propagation_speed_mps == 300.0);
    assert(run.velocity_range.bounds.v_min_mps == 250.0f);
    assert(run.velocity_range.bounds.v_max_mps == 350.0f);
    assert(run.velocity_range.bounds.vp_vs_min == 1.5f);
    assert(run.velocity_range.bounds.vp_vs_max == 2.0f);
    assert(run.velocity_range.bounds.set);
    assert(run.velocity_range.source == LOCALIZATION_VELOCITY_RANGE_SOURCE_USER);
    assert(!localization_job_state_start(&state, 5000001, &run)); /* already running */
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000002", &c, 0U, 778U) == ESP_ERR_INVALID_STATE);

    localization_job_state_progress(&state, "tdcas", false, 0U, 4U, 6000000);
    assert(state.status.progress_percent == 0U);
    localization_job_state_progress(&state, "bayesian", true, 1U, 4U, 7000000);
    assert(state.status.progress_percent == 25U);
    localization_job_state_progress(&state, "eikonal_scan_voids", false, 1U, 4U, 7500000);
    assert(state.status.progress_percent == 25U);
    localization_job_state_progress(&state, "eikonal_scan_voids", true, 2U, 4U, 7750000);
    assert(state.status.progress_percent == 50U);
    localization_job_state_snapshot(&state, 8000000, &status);
    assert(status.state == LOCALIZATION_JOB_RUNNING);
    assert(strcmp(status.current_method, "eikonal_scan_voids") == 0);
    assert(status.elapsed_ms == 3000U); /* brought up to "now" */
    localization_job_state_progress(&state, "joint_fusion", false, 3U, 4U, 8200000);
    assert(state.status.progress_percent == 75U);
    localization_job_state_progress(&state, "joint_fusion", true, 4U, 4U, 8500000);
    assert(state.status.progress_percent == 99U); /* 100 is reserved for finish(). */
    localization_job_state_progress(&state, "stale_callback", true, 1U, 9U, 8600000);
    assert(state.status.progress_percent == 99U && state.status.summary.methods_total == 4U);

    localization_calculate_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    summary.methods_total = 4U;
    summary.methods_completed = 4U;
    summary.methods_attempted = 18U;
    summary.methods_solved = 12U;
    summary.methods_skipped_for_time = 2U;
    summary.timing_count = 2U;
    snprintf(summary.timings[0].method_name, sizeof(summary.timings[0].method_name), "bayesian");
    summary.timings[0].duration_ms = 1234U;
    snprintf(summary.timings[1].method_name, sizeof(summary.timings[1].method_name), "bayesian_scan_voids");
    summary.timings[1].skipped_for_time = true;
    localization_job_state_finish(&state, ESP_OK, &summary, 9000U, 9500000);
    localization_job_state_snapshot(&state, 99000000, &status);
    assert(status.state == LOCALIZATION_JOB_SUCCEEDED);
    assert(status.progress_percent == 100U);
    assert(status.summary.methods_total == 4U && status.summary.methods_completed == 4U);
    assert(status.elapsed_ms == 4500U); /* frozen at the finish */
    assert(status.current_method[0] == '\0');
    assert(status.summary.methods_solved == 12U && status.summary.timing_count == 2U);
    assert(status.stack_free_min_bytes == 9000U && status.stack_margin_ok);

    /* Done: the next job is accepted; the stack minimum is kept across jobs. */
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000002", &c, 30000U, 800U) == ESP_OK);
    assert(state.status.job_id == 2U && state.status.budget_ms == 30000U);
    assert(state.status.summary.methods_solved == 0U);
    assert(localization_job_state_start(&state, 0, &run));
    localization_job_state_finish(&state, ESP_ERR_NO_MEM, NULL, 3000U, 1000);
    assert(state.status.state == LOCALIZATION_JOB_FAILED && state.status.error == ESP_ERR_NO_MEM);
    assert(state.status.stack_free_min_bytes == 3000U);
    assert(!state.status.stack_margin_ok); /* below LOCALIZATION_JOB_STACK_MARGIN_BYTES */
}


static void test_submit_validation(void)
{
    localization_job_state_t state;
    localization_job_state_init(&state, LOCALIZATION_JOB_STACK_BYTES);
    const localization_calculate_config_t c = config();
    assert(localization_job_state_submit(&state, "", "S000001", &c, 0U, 1U) == ESP_ERR_INVALID_ARG);
    assert(localization_job_state_submit(&state, "OBJ_0001", NULL, &c, 0U, 1U) == ESP_ERR_INVALID_ARG);
    assert(localization_job_state_submit(&state, "OBJ_0001_TOO_LONG_ID", "S000001", &c, 0U, 1U) == ESP_ERR_INVALID_ARG);
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", &c, LOCALIZATION_JOB_MIN_BUDGET_MS - 1U, 1U) == ESP_ERR_INVALID_ARG);
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", &c, LOCALIZATION_JOB_MAX_BUDGET_MS + 1U, 1U) == ESP_ERR_INVALID_ARG);
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", NULL, 0U, 1U) == ESP_ERR_INVALID_ARG);
    assert(state.status.state == LOCALIZATION_JOB_IDLE && state.status.job_id == 0U);
}


static void test_status_json(void)
{
    localization_job_state_t state;
    localization_job_state_init(&state, LOCALIZATION_JOB_STACK_BYTES);
    const localization_calculate_config_t c = config();
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", &c, 20000U, 42U) == ESP_OK);
    localization_calculate_config_t run;
    assert(localization_job_state_start(&state, 0, &run));
    localization_calculate_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    summary.timing_count = 1U;
    summary.methods_total = 5U;
    summary.methods_completed = 4U;
    summary.recommendation.action = LOCALIZATION_RECOMMENDATION_RECORD_AGAIN;
    snprintf(summary.recommendation.reason_summary, sizeof(summary.recommendation.reason_summary), "Pick evidence was weak.");
    snprintf(summary.timings[0].method_name, sizeof(summary.timings[0].method_name), "tdcas");
    summary.timings[0].duration_ms = 17U;
    localization_job_state_finish(&state, ESP_ERR_NOT_FOUND, &summary, 11000U, 2000000);

    char *json = NULL;
    assert(localization_job_status_to_json(&state.status, &json) == ESP_OK);
    printf("  %s\n", json);
    assert(strstr(json, "\"state\":\"failed\"") != NULL);
    assert(strstr(json, "\"error\":\"ESP_ERR_NOT_FOUND\"") != NULL);
    assert(strstr(json, "\"job_id\":1") != NULL);
    assert(strstr(json, "\"computed_at_ms\":42") != NULL);
    assert(strstr(json, "\"methods_total\":5") != NULL);
    assert(strstr(json, "\"methods_completed\":4") != NULL);
    assert(strstr(json, "\"progress_percent\":80") != NULL);
    assert(strstr(json, "\"successful\":false") != NULL);
    assert(strstr(json, "\"recommendation\":{\"action\":\"record_again\",\"reason_summary\":\"Pick evidence was weak.\"}") != NULL);
    assert(strstr(json, "\"elapsed_ms\":2000") != NULL && strstr(json, "\"budget_ms\":20000") != NULL);
    assert(strstr(json, "\"timings\":[{\"method\":\"tdcas\",\"ms\":17,\"skipped_for_time\":false}]") != NULL);
    assert(strstr(json, "\"stack\":{\"size_bytes\":16384,\"free_min_bytes\":11000,\"margin_bytes\":4096,\"margin_ok\":true}") != NULL);
    free(json);

    /* A running job reports no error field. */
    assert(localization_job_state_submit(&state, "OBJ_0001", "S000001", &c, 0U, 43U) == ESP_OK);
    assert(localization_job_status_to_json(&state.status, &json) == ESP_OK);
    assert(strstr(json, "\"state\":\"queued\"") != NULL && strstr(json, "\"error\"") == NULL);
    free(json);
    assert(localization_job_status_to_json(NULL, &json) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_one_job_at_a_time_with_its_deadline();
    test_submit_validation();
    test_status_json();
    printf("localization job state tests passed\n");
    return 0;
}
