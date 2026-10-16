#pragma once
#include <stdint.h>
typedef enum { RUNTIME_TASK_ACQUISITION = 0, RUNTIME_TASK_RECORDER, RUNTIME_TASK_UI, RUNTIME_TASK_COUNT } runtime_observability_task_t;
typedef enum {
    RUNTIME_HEALTH_PHASE_IDLE = 0,
    RUNTIME_HEALTH_PHASE_ACTIVE,
    RUNTIME_HEALTH_PHASE_WAITING,
    RUNTIME_HEALTH_PHASE_STOP_DRAIN,
    RUNTIME_HEALTH_PHASE_STOP_FLUSH,
    RUNTIME_HEALTH_PHASE_STOP_SYNC,
    RUNTIME_HEALTH_PHASE_STOP_CLOSE,
    RUNTIME_HEALTH_PHASE_STOP_METADATA
} runtime_health_phase_t;
typedef enum {
    RUNTIME_HEALTH_OK = 0,
    RUNTIME_HEALTH_STALE_PROGRESS,
    RUNTIME_HEALTH_DEADLINE_EXCEEDED
} runtime_health_result_t;
void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress);
void runtime_observability_set_health_phase(runtime_observability_task_t task, runtime_health_phase_t phase);
runtime_health_result_t runtime_observability_get_health_result(runtime_observability_task_t task);
