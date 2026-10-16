#include "runtime_health_policy.h"

#include <string.h>

#include "app_stop_timeout_config.h"

/* Provisional until mixed-load target measurements establish task-specific limits. */
#define RUNTIME_HEALTH_ACQUISITION_ACTIVE_MS 5000U
#define RUNTIME_HEALTH_RECORDER_ACTIVE_MS APP_STOP_TIMEOUT_RECORDING_MS
#define RUNTIME_HEALTH_STORAGE_ACTIVE_MS APP_STOP_TIMEOUT_WORKER_MS
#define RUNTIME_HEALTH_CONTROL_ACTIVE_MS APP_STOP_TIMEOUT_WORKER_MS
#define RUNTIME_HEALTH_TOPOLOGY_ACTIVE_MS APP_STOP_TIMEOUT_WORKER_MS

static bool runtime_health_is_stop_phase(runtime_health_phase_t phase)
{
    return phase >= RUNTIME_HEALTH_PHASE_STOP_DRAIN &&
        phase <= RUNTIME_HEALTH_PHASE_STOP_METADATA;
}


uint32_t runtime_health_policy_deadline_ms(
    runtime_health_task_t task,
    runtime_health_phase_t phase
)
{
    if ((unsigned)task >= RUNTIME_HEALTH_TASK_COUNT ||
        (unsigned)phase >= RUNTIME_HEALTH_PHASE_COUNT) {
        return 0U;
    }
    if (phase == RUNTIME_HEALTH_PHASE_IDLE) {
        return 0U;
    }
    if (runtime_health_is_stop_phase(phase)) {
        return APP_STOP_TIMEOUT_RECORDING_MS;
    }
    if (phase == RUNTIME_HEALTH_PHASE_WAITING) {
        return APP_STOP_TIMEOUT_WORKER_MS;
    }

    switch (task) {
        case RUNTIME_HEALTH_TASK_ACQUISITION:
            return RUNTIME_HEALTH_ACQUISITION_ACTIVE_MS;
        case RUNTIME_HEALTH_TASK_RECORDER:
            return RUNTIME_HEALTH_RECORDER_ACTIVE_MS;
        case RUNTIME_HEALTH_TASK_STORAGE:
            return RUNTIME_HEALTH_STORAGE_ACTIVE_MS;
        case RUNTIME_HEALTH_TASK_CONTROL:
            return RUNTIME_HEALTH_CONTROL_ACTIVE_MS;
        case RUNTIME_HEALTH_TASK_TOPOLOGY:
            return RUNTIME_HEALTH_TOPOLOGY_ACTIVE_MS;
        case RUNTIME_HEALTH_TASK_COUNT:
        default:
            return 0U;
    }
}


runtime_health_result_t runtime_health_policy_evaluate(
    runtime_health_task_t task,
    runtime_health_phase_t phase,
    const runtime_health_observation_t *observation,
    uint64_t now_ms
)
{
    const uint32_t deadline_ms = runtime_health_policy_deadline_ms(task, phase);
    if (deadline_ms == 0U || observation == NULL || !observation->observed) {
        return RUNTIME_HEALTH_OK;
    }

    const uint64_t start_ms = (phase == RUNTIME_HEALTH_PHASE_WAITING ||
        runtime_health_is_stop_phase(phase))
        ? observation->phase_started_ms
        : observation->last_progress_ms;
    if (now_ms < start_ms) {
        return RUNTIME_HEALTH_OK;
    }
    const uint64_t age_ms = now_ms - start_ms;
    if (age_ms >= deadline_ms) {
        return RUNTIME_HEALTH_DEADLINE_EXCEEDED;
    }
    if (phase != RUNTIME_HEALTH_PHASE_WAITING && age_ms >= deadline_ms / 2U) {
        return RUNTIME_HEALTH_STALE_PROGRESS;
    }
    return RUNTIME_HEALTH_OK;
}


runtime_health_reaction_t runtime_health_policy_reaction(
    runtime_health_result_t result,
    bool recording_active,
    bool recording_incomplete,
    bool hard_failure
)
{
    if (hard_failure) {
        return RUNTIME_HEALTH_REACTION_REBOOT;
    }
    if (result != RUNTIME_HEALTH_DEADLINE_EXCEEDED) {
        return RUNTIME_HEALTH_REACTION_DIAGNOSE;
    }
    if (recording_active && !recording_incomplete) {
        return RUNTIME_HEALTH_REACTION_MARK_INCOMPLETE;
    }
    return RUNTIME_HEALTH_REACTION_SAFE_STOP;
}


runtime_health_task_set_result_t runtime_health_policy_check_task_set(
    const char *const *expected_names,
    size_t expected_count,
    const char *const *observed_names,
    size_t observed_count
)
{
    if ((expected_count > 0U && expected_names == NULL) ||
        (observed_count > 0U && observed_names == NULL)) {
        return RUNTIME_HEALTH_TASK_SET_BLOCKED;
    }

    for (size_t i = 0U; i < expected_count; ++i) {
        if (expected_names[i] == NULL || expected_names[i][0] == '\0') {
            return RUNTIME_HEALTH_TASK_SET_BLOCKED;
        }
        bool found = false;
        for (size_t j = 0U; j < observed_count; ++j) {
            if (observed_names[j] != NULL &&
                strcmp(expected_names[i], observed_names[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return RUNTIME_HEALTH_TASK_SET_BLOCKED;
        }
    }
    return RUNTIME_HEALTH_TASK_SET_COMPLETE;
}
