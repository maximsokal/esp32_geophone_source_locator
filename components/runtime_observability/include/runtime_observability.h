#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef enum
{
    RUNTIME_TASK_ACQUISITION = 0,
    RUNTIME_TASK_RECORDER,
    RUNTIME_TASK_UI,
    RUNTIME_TASK_KEYPAD,
    RUNTIME_TASK_POWER_BUTTON,
    RUNTIME_TASK_COUNT

} runtime_observability_task_t;


typedef struct
{
    bool observed;
    uint64_t heartbeat;
    uint64_t progress;
    uint64_t last_heartbeat_ms;

} runtime_observability_task_status_t;


typedef struct
{
    /* Device monotonic time at which this snapshot was copied. */
    uint64_t now_ms;
    runtime_observability_task_status_t
        tasks[RUNTIME_TASK_COUNT];

} runtime_observability_snapshot_t;


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


void runtime_observability_get_snapshot(
    runtime_observability_snapshot_t *snapshot
);


const char *runtime_observability_task_name(
    runtime_observability_task_t task
);
