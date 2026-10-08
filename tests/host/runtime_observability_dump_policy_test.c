#include "runtime_observability_dump_policy.h"

#include <assert.h>
#include <stdint.h>


static uint32_t decide(
    runtime_observability_dump_policy_state_t *state,
    uint32_t now_ms,
    uint32_t internal_free,
    uint32_t internal_largest,
    const runtime_observability_dump_task_t *tasks,
    size_t task_count
)
{
    return runtime_observability_dump_decide(
        state, now_ms, internal_free, internal_largest, tasks, task_count
    );
}


static void test_first_identical_and_heartbeat(void)
{
    runtime_observability_dump_policy_state_t state = {0};
    runtime_observability_dump_task_t tasks[1] = { { 1U, 1024U, false } };

    assert(decide(&state, 0U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_FIRST);
    assert(decide(&state, 10000U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 59999U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 60000U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_HEARTBEAT);
    assert(decide(&state, 119999U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 120000U, 30000U, 20000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_HEARTBEAT);
}


static void test_low_heap_thresholds_and_rearming(void)
{
    runtime_observability_dump_policy_state_t state = {0};

    assert(decide(&state, 0U, 20480U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_FIRST);
    assert(decide(&state, 10000U, 20479U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP);
    assert(decide(&state, 20000U, 20479U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 30000U, 20480U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 40000U, 20479U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP);

    assert(decide(&state, 50000U, 20480U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 60000U, 20480U, 16383U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP);
    assert(decide(&state, 70000U, 20480U, 16384U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, 80000U, 20480U, 16383U, NULL, 0U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_HEAP);
}


static void test_low_stack_edges_rearm_per_task(void)
{
    runtime_observability_dump_policy_state_t state = {0};
    runtime_observability_dump_task_t task = { 1U, 512U, false };

    assert(decide(&state, 0U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_FIRST);
    task.stack_free_bytes = 511U;
    assert(decide(&state, 10000U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_STACK);
    assert(decide(&state, 20000U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    task.stack_free_bytes = 512U;
    assert(decide(&state, 30000U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_NONE);
    task.stack_free_bytes = 511U;
    assert(decide(&state, 40000U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_STACK);
    task.task_handle = 2U;
    assert(decide(&state, 50000U, 30000U, 20000U, &task, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_LOW_STACK);
}


static void test_wraparound_and_logged_snapshot(void)
{
    runtime_observability_dump_policy_state_t state = {0};
    runtime_observability_dump_task_t tasks[1] = { { 7U, 600U, false } };
    const uint32_t start_ms = UINT32_MAX - 1000U;

    assert(decide(&state, start_ms, 25000U, 18000U, tasks, 1U) ==
           RUNTIME_OBSERVABILITY_DUMP_FIRST);
    assert(state.last_dump_ms == start_ms);
    assert(state.last_dump_internal_free == 25000U);
    assert(state.last_dump_internal_largest == 18000U);
    assert(state.last_dump_task_count == 1U);
    assert(state.last_dump_tasks[0].task_handle == 7U);
    assert(decide(&state, (uint32_t)(start_ms + 59999U), 25000U, 18000U,
                  tasks, 1U) == RUNTIME_OBSERVABILITY_DUMP_NONE);
    assert(decide(&state, (uint32_t)(start_ms + 60000U), 25000U, 18000U,
                  tasks, 1U) == RUNTIME_OBSERVABILITY_DUMP_HEARTBEAT);
}


int main(void)
{
    test_first_identical_and_heartbeat();
    test_low_heap_thresholds_and_rearming();
    test_low_stack_edges_rearm_per_task();
    test_wraparound_and_logged_snapshot();
    return 0;
}
