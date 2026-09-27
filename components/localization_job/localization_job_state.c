#include "localization_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"


const char *localization_job_state_name(localization_job_state_id_t state)
{
    switch (state) {
        case LOCALIZATION_JOB_QUEUED: return "queued";
        case LOCALIZATION_JOB_RUNNING: return "running";
        case LOCALIZATION_JOB_SUCCEEDED: return "succeeded";
        case LOCALIZATION_JOB_FAILED: return "failed";
        case LOCALIZATION_JOB_IDLE:
        default: return "idle";
    }
}


static const char *error_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK: return "ESP_OK";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "ESP_FAIL";
    }
}


static bool copy_id(char *out, const char *id)
{
    if (id == NULL || id[0] == '\0' || strlen(id) >= LOCALIZATION_JOB_ID_SIZE) {
        return false;
    }
    snprintf(out, LOCALIZATION_JOB_ID_SIZE, "%s", id);
    return true;
}


void localization_job_state_init(localization_job_state_t *state, uint32_t stack_size_bytes)
{
    memset(state, 0, sizeof(*state));
    state->status.state = LOCALIZATION_JOB_IDLE;
    state->status.stack_size_bytes = stack_size_bytes;
    state->status.stack_margin_ok = true;
}


esp_err_t localization_job_state_submit(
    localization_job_state_t *state,
    const char *object_id,
    const char *session_id,
    const localization_calculate_config_t *config,
    uint32_t budget_ms,
    uint64_t computed_at_ms
)
{
    if (state == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->status.state == LOCALIZATION_JOB_QUEUED || state->status.state == LOCALIZATION_JOB_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (budget_ms == 0U) {
        budget_ms = LOCALIZATION_JOB_DEFAULT_BUDGET_MS;
    }
    char object[LOCALIZATION_JOB_ID_SIZE];
    char session[LOCALIZATION_JOB_ID_SIZE];
    if (
        !copy_id(object, object_id) || !copy_id(session, session_id) ||
        budget_ms < LOCALIZATION_JOB_MIN_BUDGET_MS || budget_ms > LOCALIZATION_JOB_MAX_BUDGET_MS
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    localization_job_status_t *status = &state->status;
    const uint32_t stack_size = status->stack_size_bytes;
    const uint32_t stack_free = status->stack_free_min_bytes;
    const bool margin_ok = status->stack_margin_ok;
    const uint32_t next_id = status->job_id + 1U;
    memset(status, 0, sizeof(*status));
    status->state = LOCALIZATION_JOB_QUEUED;
    status->job_id = next_id;
    memcpy(status->object_id, object, sizeof(object));
    memcpy(status->session_id, session, sizeof(session));
    status->computed_at_ms = computed_at_ms;
    status->budget_ms = budget_ms;
    /* The stack figures are the worker's lifetime minimum: kept across jobs. */
    status->stack_size_bytes = stack_size;
    status->stack_free_min_bytes = stack_free;
    status->stack_margin_ok = margin_ok;
    state->config = *config;
    return ESP_OK;
}


bool localization_job_state_start(
    localization_job_state_t *state,
    int64_t now_us,
    localization_calculate_config_t *config_out
)
{
    if (state->status.state != LOCALIZATION_JOB_QUEUED) {
        return false;
    }
    state->status.state = LOCALIZATION_JOB_RUNNING;
    state->started_us = now_us;
    *config_out = state->config;
    config_out->deadline_us = now_us + (int64_t)state->status.budget_ms * 1000;
    return true;
}


static uint32_t elapsed_ms(int64_t from_us, int64_t to_us)
{
    const int64_t ms = (to_us - from_us) / 1000;
    return ms > 0 ? (uint32_t)(ms > (int64_t)UINT32_MAX ? UINT32_MAX : ms) : 0U;
}


static const char *recommendation_action_name(localization_recommendation_action_t action)
{
    switch (action) {
        case LOCALIZATION_RECOMMENDATION_RECORD_AGAIN: return "record_again";
        case LOCALIZATION_RECOMMENDATION_MOVE_SENSORS: return "move_sensors";
        case LOCALIZATION_RECOMMENDATION_NONE:
        default: return "none";
    }
}


static uint32_t progress_percent(size_t completed, size_t total)
{
    if (total == 0U) {
        return 0U;
    }
    if (completed > total) {
        completed = total;
    }
    return (uint32_t)(((uint64_t)completed * 100U) / total);
}


void localization_job_state_progress(
    localization_job_state_t *state,
    const char *method_name,
    bool completed,
    size_t methods_completed,
    size_t methods_total,
    int64_t now_us
)
{
    if (state->status.state != LOCALIZATION_JOB_RUNNING) {
        return;
    }
    if (!completed) {
        snprintf(state->status.current_method, sizeof(state->status.current_method), "%s",
                 method_name != NULL ? method_name : "");
    }
    if (state->status.summary.methods_total != 0U) {
        methods_total = state->status.summary.methods_total;
    }
    if (methods_completed < state->status.summary.methods_completed) {
        methods_completed = state->status.summary.methods_completed;
    }
    state->status.summary.methods_total = methods_total;
    state->status.summary.methods_completed = methods_completed;
    const uint32_t calculated_percent = progress_percent(methods_completed, methods_total);
    const uint32_t running_percent = calculated_percent >= 100U ? 99U : calculated_percent;
    if (running_percent > state->status.progress_percent) {
        state->status.progress_percent = running_percent;
    }
    state->status.elapsed_ms = elapsed_ms(state->started_us, now_us);
}


void localization_job_state_finish(
    localization_job_state_t *state,
    esp_err_t result,
    const localization_calculate_summary_t *summary,
    uint32_t stack_free_min_bytes,
    int64_t now_us
)
{
    localization_job_status_t *status = &state->status;
    if (status->state != LOCALIZATION_JOB_RUNNING) {
        return;
    }
    status->state = result == ESP_OK ? LOCALIZATION_JOB_SUCCEEDED : LOCALIZATION_JOB_FAILED;
    status->error = result;
    status->current_method[0] = '\0';
    status->elapsed_ms = elapsed_ms(state->started_us, now_us);
    const size_t previous_methods_total = status->summary.methods_total;
    const size_t previous_methods_completed = status->summary.methods_completed;
    if (summary != NULL) {
        status->summary = *summary;
        if (previous_methods_total != 0U) {
            status->summary.methods_total = previous_methods_total;
            if (status->summary.methods_completed < previous_methods_completed) {
                status->summary.methods_completed = previous_methods_completed;
            }
        }
    }
    const uint32_t finished_percent = progress_percent(
        status->summary.methods_completed, status->summary.methods_total
    );
    if (finished_percent > status->progress_percent) {
        status->progress_percent = finished_percent;
    }
    if (status->stack_free_min_bytes == 0U || stack_free_min_bytes < status->stack_free_min_bytes) {
        status->stack_free_min_bytes = stack_free_min_bytes;
    }
    status->stack_margin_ok = status->stack_free_min_bytes >= LOCALIZATION_JOB_STACK_MARGIN_BYTES;
}


void localization_job_state_snapshot(
    const localization_job_state_t *state,
    int64_t now_us,
    localization_job_status_t *status_out
)
{
    *status_out = state->status;
    if (state->status.state == LOCALIZATION_JOB_RUNNING) {
        status_out->elapsed_ms = elapsed_ms(state->started_us, now_us);
    }
}


esp_err_t localization_job_status_to_json(const localization_job_status_t *status, char **json_out)
{
    if (status == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok =
        cJSON_AddStringToObject(root, "state", localization_job_state_name(status->state)) != NULL &&
        cJSON_AddNumberToObject(root, "job_id", status->job_id) != NULL &&
        cJSON_AddStringToObject(root, "object_id", status->object_id) != NULL &&
        cJSON_AddStringToObject(root, "session_id", status->session_id) != NULL &&
        cJSON_AddNumberToObject(root, "computed_at_ms", (double)status->computed_at_ms) != NULL &&
        cJSON_AddStringToObject(root, "current_method", status->current_method) != NULL &&
        cJSON_AddNumberToObject(root, "elapsed_ms", status->elapsed_ms) != NULL &&
        cJSON_AddNumberToObject(root, "budget_ms", status->budget_ms) != NULL &&
        cJSON_AddNumberToObject(root, "methods_total", (double)status->summary.methods_total) != NULL &&
        cJSON_AddNumberToObject(root, "methods_completed", (double)status->summary.methods_completed) != NULL &&
        cJSON_AddNumberToObject(root, "progress_percent", status->progress_percent) != NULL;
    if (ok && status->state == LOCALIZATION_JOB_FAILED) {
        ok = cJSON_AddStringToObject(root, "error", error_name(status->error)) != NULL;
    }

    cJSON *summary = ok ? cJSON_AddObjectToObject(root, "summary") : NULL;
    ok = summary != NULL &&
        cJSON_AddNumberToObject(summary, "participating_channel_count", (double)status->summary.participating_channel_count) != NULL &&
        cJSON_AddNumberToObject(summary, "methods_attempted", (double)status->summary.methods_attempted) != NULL &&
        cJSON_AddNumberToObject(summary, "methods_solved", (double)status->summary.methods_solved) != NULL &&
        cJSON_AddNumberToObject(summary, "methods_persisted", (double)status->summary.methods_persisted) != NULL &&
        cJSON_AddNumberToObject(summary, "methods_skipped_for_time", (double)status->summary.methods_skipped_for_time) != NULL &&
        cJSON_AddBoolToObject(summary, "successful", status->summary.recommendation.successful) != NULL;
    cJSON *recommendation = ok ? cJSON_AddObjectToObject(summary, "recommendation") : NULL;
    ok = recommendation != NULL &&
        cJSON_AddStringToObject(
            recommendation, "action", recommendation_action_name(status->summary.recommendation.action)
        ) != NULL &&
        cJSON_AddStringToObject(
            recommendation, "reason_summary", status->summary.recommendation.reason_summary
        ) != NULL;
    cJSON *timings = ok ? cJSON_AddArrayToObject(summary, "timings") : NULL;
    ok = ok && timings != NULL;
    const size_t count = status->summary.timing_count < LOCALIZATION_CALCULATE_MAX_TIMINGS
        ? status->summary.timing_count : LOCALIZATION_CALCULATE_MAX_TIMINGS;
    for (size_t i = 0U; ok && i < count; ++i) {
        const localization_calculate_method_timing_t *t = &status->summary.timings[i];
        cJSON *entry = cJSON_CreateObject();
        ok = entry != NULL &&
            cJSON_AddStringToObject(entry, "method", t->method_name) != NULL &&
            cJSON_AddNumberToObject(entry, "ms", t->duration_ms) != NULL &&
            cJSON_AddBoolToObject(entry, "skipped_for_time", t->skipped_for_time) != NULL;
        if (entry != NULL) {
            cJSON_AddItemToArray(timings, entry);
        }
    }

    cJSON *stack = ok ? cJSON_AddObjectToObject(root, "stack") : NULL;
    ok = stack != NULL &&
        cJSON_AddNumberToObject(stack, "size_bytes", status->stack_size_bytes) != NULL &&
        cJSON_AddNumberToObject(stack, "free_min_bytes", status->stack_free_min_bytes) != NULL &&
        cJSON_AddNumberToObject(stack, "margin_bytes", LOCALIZATION_JOB_STACK_MARGIN_BYTES) != NULL &&
        cJSON_AddBoolToObject(stack, "margin_ok", status->stack_margin_ok) != NULL;

    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *json_out = json;
    return ESP_OK;
}
