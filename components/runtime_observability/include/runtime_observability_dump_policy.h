#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define RUNTIME_OBSERVABILITY_HEARTBEAT_PERIOD_MS 60000U
#define RUNTIME_OBSERVABILITY_DUMP_INTERNAL_FREE_LOW_BYTES 20480U
#define RUNTIME_OBSERVABILITY_DUMP_INTERNAL_LARGEST_LOW_BYTES 16384U
#define RUNTIME_OBSERVABILITY_DUMP_LOW_STACK_BYTES 512U
/* Match the maximum number of rows in the emitted top-task table. */
#define RUNTIME_OBSERVABILITY_DUMP_TRACKED_TASKS 10U


enum
{
    RUNTIME_OBSERVABILITY_DUMP_NONE = 0U,
    RUNTIME_OBSERVABILITY_DUMP_FIRST = 1U,
    RUNTIME_OBSERVABILITY_DUMP_HEARTBEAT = 2U,
    RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP = 4U,
    RUNTIME_OBSERVABILITY_DUMP_LOW_STACK = 8U
};


typedef struct
{
    uintptr_t task_handle;
    uint32_t stack_free_bytes;
    bool low_stack;
} runtime_observability_dump_task_t;


typedef struct
{
    bool has_evaluated;
    bool previous_low_heap;
    size_t previous_task_count;
    runtime_observability_dump_task_t previous_tasks[
        RUNTIME_OBSERVABILITY_DUMP_TRACKED_TASKS
    ];
    bool has_dumped;
    uint32_t last_dump_ms;
    uint32_t last_dump_internal_free;
    uint32_t last_dump_internal_largest;
    size_t last_dump_task_count;
    runtime_observability_dump_task_t last_dump_tasks[
        RUNTIME_OBSERVABILITY_DUMP_TRACKED_TASKS
    ];
} runtime_observability_dump_policy_state_t;


static inline size_t runtime_observability_dump_task_count(
    size_t task_count
)
{
    return task_count < RUNTIME_OBSERVABILITY_DUMP_TRACKED_TASKS
        ? task_count
        : RUNTIME_OBSERVABILITY_DUMP_TRACKED_TASKS;
}


static inline bool runtime_observability_dump_has_previous_low_task(
    const runtime_observability_dump_policy_state_t *state,
    uintptr_t task_handle
)
{
    size_t i;

    for (i = 0U; i < state->previous_task_count; ++i) {
        if (
            state->previous_tasks[i].task_handle == task_handle &&
            state->previous_tasks[i].low_stack
        ) {
            return true;
        }
    }
    return false;
}


static inline void runtime_observability_dump_record_evaluation(
    runtime_observability_dump_policy_state_t *state,
    uint32_t internal_free,
    uint32_t internal_largest,
    const runtime_observability_dump_task_t *tasks,
    size_t task_count
)
{
    size_t i;

    task_count = runtime_observability_dump_task_count(task_count);
    state->has_evaluated = true;
    state->previous_low_heap =
        internal_free < RUNTIME_OBSERVABILITY_DUMP_INTERNAL_FREE_LOW_BYTES ||
        internal_largest < RUNTIME_OBSERVABILITY_DUMP_INTERNAL_LARGEST_LOW_BYTES;
    state->previous_task_count = task_count;
    for (i = 0U; i < task_count; ++i) {
        state->previous_tasks[i].task_handle = tasks[i].task_handle;
        state->previous_tasks[i].stack_free_bytes = tasks[i].stack_free_bytes;
        state->previous_tasks[i].low_stack =
            tasks[i].stack_free_bytes < RUNTIME_OBSERVABILITY_DUMP_LOW_STACK_BYTES;
    }
}


static inline void runtime_observability_dump_record_snapshot(
    runtime_observability_dump_policy_state_t *state,
    uint32_t now_ms,
    uint32_t internal_free,
    uint32_t internal_largest,
    const runtime_observability_dump_task_t *tasks,
    size_t task_count
)
{
    size_t i;

    task_count = runtime_observability_dump_task_count(task_count);
    state->has_dumped = true;
    state->last_dump_ms = now_ms;
    state->last_dump_internal_free = internal_free;
    state->last_dump_internal_largest = internal_largest;
    state->last_dump_task_count = task_count;
    for (i = 0U; i < task_count; ++i) {
        state->last_dump_tasks[i] = tasks[i];
        state->last_dump_tasks[i].low_stack =
            tasks[i].stack_free_bytes < RUNTIME_OBSERVABILITY_DUMP_LOW_STACK_BYTES;
    }
}


static inline void runtime_observability_dump_record_logged(
    runtime_observability_dump_policy_state_t *state,
    uint32_t now_ms,
    uint32_t internal_free,
    uint32_t internal_largest,
    const runtime_observability_dump_task_t *tasks,
    size_t task_count
)
{
    runtime_observability_dump_record_evaluation(
        state, internal_free, internal_largest, tasks, task_count
    );
    runtime_observability_dump_record_snapshot(
        state, now_ms, internal_free, internal_largest, tasks, task_count
    );
}


static inline uint32_t runtime_observability_dump_decide(
    runtime_observability_dump_policy_state_t *state,
    uint32_t now_ms,
    uint32_t internal_free,
    uint32_t internal_largest,
    const runtime_observability_dump_task_t *tasks,
    size_t task_count
)
{
    size_t i;
    uint32_t reason;
    bool low_heap;
    bool low_heap_edge;
    bool low_stack_edge;
    bool heartbeat_due;

    task_count = runtime_observability_dump_task_count(task_count);
    low_heap =
        internal_free < RUNTIME_OBSERVABILITY_DUMP_INTERNAL_FREE_LOW_BYTES ||
        internal_largest < RUNTIME_OBSERVABILITY_DUMP_INTERNAL_LARGEST_LOW_BYTES;
    low_heap_edge = state->has_evaluated && low_heap && !state->previous_low_heap;
    low_stack_edge = false;
    if (state->has_evaluated) {
        for (i = 0U; i < task_count; ++i) {
            if (
                tasks[i].stack_free_bytes < RUNTIME_OBSERVABILITY_DUMP_LOW_STACK_BYTES &&
                !runtime_observability_dump_has_previous_low_task(
                    state, tasks[i].task_handle
                )
            ) {
                low_stack_edge = true;
            }
        }
    }

    heartbeat_due = state->has_dumped &&
        (uint32_t)(now_ms - state->last_dump_ms) >=
            RUNTIME_OBSERVABILITY_HEARTBEAT_PERIOD_MS;
    reason = RUNTIME_OBSERVABILITY_DUMP_NONE;
    if (!state->has_dumped) {
        reason |= RUNTIME_OBSERVABILITY_DUMP_FIRST;
    }
    if (heartbeat_due) {
        reason |= RUNTIME_OBSERVABILITY_DUMP_HEARTBEAT;
    }
    if (low_heap_edge) {
        reason |= RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP;
    }
    if (low_stack_edge) {
        reason |= RUNTIME_OBSERVABILITY_DUMP_LOW_STACK;
    }

    runtime_observability_dump_record_evaluation(
        state, internal_free, internal_largest, tasks, task_count
    );
    if (reason != RUNTIME_OBSERVABILITY_DUMP_NONE) {
        runtime_observability_dump_record_snapshot(
            state, now_ms, internal_free, internal_largest, tasks, task_count
        );
    }
    return reason;
}
