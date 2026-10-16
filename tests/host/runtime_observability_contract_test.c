#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "runtime_observability.h"
#include "runtime_health_policy.h"


static int64_t s_now_us;


int64_t esp_timer_get_time(void)
{
    return s_now_us;
}


int main(void)
{
    runtime_observability_heap_snapshot_t heap = { 1U, 1U, 1U, 1U, 1U, 1U };
    runtime_observability_get_heap_snapshot(&heap);
    assert(heap.free_internal == 0U);
    assert(heap.largest_internal_block == 0U);
    assert(heap.free_dma == 0U);
    assert(heap.largest_dma_block == 0U);
    assert(heap.free_psram == 0U);
    assert(heap.min_free_internal_since_boot == 0U);

    runtime_observability_snapshot_t before = {0};
    runtime_observability_snapshot_t after = {0};

    runtime_observability_get_snapshot(&before);
    for (int i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        assert(!before.tasks[i].observed);
        assert(before.tasks[i].heartbeat == 0U);
    }

    s_now_us = 1234567;
    runtime_observability_set_health_phase(
        RUNTIME_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE
    );
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, 11U);
    runtime_observability_beat(RUNTIME_TASK_RECORDER, 7U);
    runtime_observability_beat(RUNTIME_TASK_UI, 0U);

    s_now_us = 2234567;
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, 12U);
    runtime_observability_beat(RUNTIME_TASK_RECORDER, 8U);

    s_now_us = 6234567;
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, 12U);

    runtime_observability_get_snapshot(&after);
    assert(after.now_ms == 6234U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].observed);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].heartbeat == 3U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].progress == 12U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].last_heartbeat_ms == 6234U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].last_progress_ms == 2234U);
    const runtime_health_observation_t frozen = {
        .observed = after.tasks[RUNTIME_TASK_ACQUISITION].observed,
        .heartbeat = after.tasks[RUNTIME_TASK_ACQUISITION].heartbeat,
        .progress = after.tasks[RUNTIME_TASK_ACQUISITION].progress,
        .last_progress_ms = after.tasks[RUNTIME_TASK_ACQUISITION].last_progress_ms,
        .phase_started_ms = 1234U,
    };
    assert(runtime_health_policy_evaluate(
        RUNTIME_HEALTH_TASK_ACQUISITION,
        RUNTIME_HEALTH_PHASE_ACTIVE,
        &frozen,
        after.now_ms
    ) == RUNTIME_HEALTH_STALE_PROGRESS);
    assert(runtime_observability_get_health_result(
        RUNTIME_TASK_ACQUISITION
    ) == RUNTIME_HEALTH_STALE_PROGRESS);
    assert(after.tasks[RUNTIME_TASK_RECORDER].heartbeat == 2U);
    assert(after.tasks[RUNTIME_TASK_RECORDER].progress == 8U);
    assert(after.tasks[RUNTIME_TASK_UI].heartbeat == 1U);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_UI), "ui") == 0);

    runtime_observability_beat((runtime_observability_task_t)-1, 99U);
    runtime_observability_beat(RUNTIME_TASK_COUNT, 99U);
    runtime_observability_snapshot_t unchanged = {0};
    runtime_observability_get_snapshot(&unchanged);
    assert(unchanged.tasks[RUNTIME_TASK_UI].heartbeat == 1U);
    assert(unchanged.tasks[RUNTIME_TASK_UI].heartbeat == 1U);

    s_now_us = 10000000;
    runtime_observability_set_health_phase(
        RUNTIME_TASK_STORAGE,
        RUNTIME_HEALTH_PHASE_WAITING
    );
    runtime_observability_beat(RUNTIME_TASK_STORAGE, 0U);
    s_now_us = 14999000;
    assert(runtime_observability_get_health_result(RUNTIME_TASK_STORAGE) == RUNTIME_HEALTH_OK);
    s_now_us = 15000000;
    assert(runtime_observability_get_health_result(RUNTIME_TASK_STORAGE) == RUNTIME_HEALTH_DEADLINE_EXCEEDED);

    runtime_observability_set_health_phase(
        RUNTIME_TASK_CONTROL,
        RUNTIME_HEALTH_PHASE_ACTIVE
    );
    runtime_observability_beat(RUNTIME_TASK_CONTROL, 1U);
    s_now_us = 17500000;
    assert(runtime_observability_get_health_result(RUNTIME_TASK_CONTROL) == RUNTIME_HEALTH_STALE_PROGRESS);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_STORAGE), "storage") == 0);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_CONTROL), "control") == 0);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_TOPOLOGY), "topology") == 0);

    return 0;
}
