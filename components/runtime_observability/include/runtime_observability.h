#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime_health_policy.h"


typedef enum
{
    RUNTIME_TASK_ACQUISITION = 0,
    RUNTIME_TASK_RECORDER,
    RUNTIME_TASK_STORAGE,
    RUNTIME_TASK_CONTROL,
    RUNTIME_TASK_TOPOLOGY,
    RUNTIME_TASK_UI,
    RUNTIME_TASK_COUNT

} runtime_observability_task_t;


typedef struct
{
    bool observed;
    uint64_t heartbeat;
    uint64_t progress;
    uint64_t last_heartbeat_ms;
    uint64_t last_progress_ms;
    runtime_health_phase_t health_phase;
    uint64_t phase_started_ms;

} runtime_observability_task_status_t;


typedef struct
{
    /* Device monotonic time at which this snapshot was copied. */
    uint64_t now_ms;
    runtime_observability_task_status_t
        tasks[RUNTIME_TASK_COUNT];

} runtime_observability_snapshot_t;

typedef struct
{
    size_t free_internal;
    size_t largest_internal_block;
    size_t free_dma;
    size_t largest_dma_block;
    size_t free_psram;
    size_t min_free_internal_since_boot;
} runtime_observability_heap_snapshot_t;


/*
 * Record one completed service point in a task's existing loop. This update
 * only takes a short critical section and never waits on a FreeRTOS object.
 * progress is task-specific monotonic work (samples, frames, events, or
 * polls); heartbeat advances even when no work was available.
 */
void runtime_observability_beat(
    runtime_observability_task_t task,
    uint64_t progress
);

void runtime_observability_set_health_phase(
    runtime_observability_task_t task,
    runtime_health_phase_t phase
);

runtime_health_result_t runtime_observability_get_health_result(
    runtime_observability_task_t task
);


void runtime_observability_get_snapshot(
    runtime_observability_snapshot_t *snapshot
);

/* Cheap heap-capability counters for the status endpoint; never allocates. */
void runtime_observability_get_heap_snapshot(
    runtime_observability_heap_snapshot_t *snapshot
);


const char *runtime_observability_task_name(
    runtime_observability_task_t task
);


/*
 * Start/stop the low-priority diagnostics task. The target implementation
 * allocates its task stack and task-status buffers from PSRAM; host builds
 * provide no-op implementations and therefore make no claims about RTOS
 * stack behaviour.
 */
bool runtime_observability_start(void);
void runtime_observability_stop(void);


/*
 * Capture a raw FreeRTOS task/runtime snapshot and a capability-aware heap
 * snapshot. Call this at lifecycle events or from the 10-second diagnostics
 * task; do not call it from a realtime acquisition loop.
 */
void runtime_observability_log_snapshot(const char *stage);
void runtime_observability_log_memory(const char *stage);

/* Queue an event snapshot without making the caller run the diagnostics path. */
void runtime_observability_request_snapshot(const char *stage);
