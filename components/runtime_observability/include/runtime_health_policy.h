#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    RUNTIME_HEALTH_TASK_ACQUISITION = 0,
    RUNTIME_HEALTH_TASK_RECORDER,
    RUNTIME_HEALTH_TASK_STORAGE,
    RUNTIME_HEALTH_TASK_CONTROL,
    RUNTIME_HEALTH_TASK_TOPOLOGY,
    RUNTIME_HEALTH_TASK_COUNT
} runtime_health_task_t;

typedef enum
{
    RUNTIME_HEALTH_PHASE_IDLE = 0,
    RUNTIME_HEALTH_PHASE_ACTIVE,
    RUNTIME_HEALTH_PHASE_WAITING,
    RUNTIME_HEALTH_PHASE_STOP_DRAIN,
    RUNTIME_HEALTH_PHASE_STOP_FLUSH,
    RUNTIME_HEALTH_PHASE_STOP_SYNC,
    RUNTIME_HEALTH_PHASE_STOP_CLOSE,
    RUNTIME_HEALTH_PHASE_STOP_METADATA,
    RUNTIME_HEALTH_PHASE_COUNT
} runtime_health_phase_t;

typedef enum
{
    RUNTIME_HEALTH_OK = 0,
    RUNTIME_HEALTH_STALE_PROGRESS,
    RUNTIME_HEALTH_DEADLINE_EXCEEDED
} runtime_health_result_t;

typedef enum
{
    RUNTIME_HEALTH_REACTION_DIAGNOSE = 0,
    RUNTIME_HEALTH_REACTION_MARK_INCOMPLETE,
    RUNTIME_HEALTH_REACTION_SAFE_STOP,
    RUNTIME_HEALTH_REACTION_REBOOT
} runtime_health_reaction_t;

typedef enum
{
    RUNTIME_HEALTH_TASK_SET_COMPLETE = 0,
    RUNTIME_HEALTH_TASK_SET_BLOCKED
} runtime_health_task_set_result_t;

typedef struct
{
    bool observed;
    uint64_t heartbeat;
    uint64_t progress;
    uint64_t last_progress_ms;
    uint64_t phase_started_ms;
} runtime_health_observation_t;

uint32_t runtime_health_policy_deadline_ms(
    runtime_health_task_t task,
    runtime_health_phase_t phase
);

runtime_health_result_t runtime_health_policy_evaluate(
    runtime_health_task_t task,
    runtime_health_phase_t phase,
    const runtime_health_observation_t *observation,
    uint64_t now_ms
);

runtime_health_reaction_t runtime_health_policy_reaction(
    runtime_health_result_t result,
    bool recording_active,
    bool recording_incomplete,
    bool hard_failure
);

runtime_health_task_set_result_t runtime_health_policy_check_task_set(
    const char *const *expected_names,
    size_t expected_count,
    const char *const *observed_names,
    size_t observed_count
);
