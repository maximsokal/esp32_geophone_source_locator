#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "runtime_observability.h"


static int64_t s_now_us;


int64_t esp_timer_get_time(void)
{
    return s_now_us;
}


int main(void)
{
    runtime_observability_snapshot_t before = {0};
    runtime_observability_snapshot_t after = {0};

    runtime_observability_get_snapshot(&before);
    for (int i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        assert(!before.tasks[i].observed);
        assert(before.tasks[i].heartbeat == 0U);
    }

    s_now_us = 1234567;
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, 11U);
    runtime_observability_beat(RUNTIME_TASK_RECORDER, 7U);
    runtime_observability_beat(RUNTIME_TASK_UI, 0U);
    runtime_observability_beat(RUNTIME_TASK_KEYPAD, 42U);
    runtime_observability_beat(RUNTIME_TASK_POWER_BUTTON, 3U);

    s_now_us = 2234567;
    runtime_observability_beat(RUNTIME_TASK_ACQUISITION, 12U);
    runtime_observability_beat(RUNTIME_TASK_RECORDER, 8U);

    runtime_observability_get_snapshot(&after);
    assert(after.now_ms == 2234U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].observed);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].heartbeat == 2U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].progress == 12U);
    assert(after.tasks[RUNTIME_TASK_ACQUISITION].last_heartbeat_ms == 2234U);
    assert(after.tasks[RUNTIME_TASK_RECORDER].heartbeat == 2U);
    assert(after.tasks[RUNTIME_TASK_RECORDER].progress == 8U);
    assert(after.tasks[RUNTIME_TASK_UI].heartbeat == 1U);
    assert(after.tasks[RUNTIME_TASK_KEYPAD].progress == 42U);
    assert(after.tasks[RUNTIME_TASK_POWER_BUTTON].progress == 3U);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_UI), "ui") == 0);
    assert(strcmp(runtime_observability_task_name(RUNTIME_TASK_POWER_BUTTON), "power_button") == 0);

    runtime_observability_beat((runtime_observability_task_t)-1, 99U);
    runtime_observability_beat(RUNTIME_TASK_COUNT, 99U);
    runtime_observability_snapshot_t unchanged = {0};
    runtime_observability_get_snapshot(&unchanged);
    assert(unchanged.tasks[RUNTIME_TASK_UI].heartbeat == 1U);
    assert(unchanged.tasks[RUNTIME_TASK_POWER_BUTTON].heartbeat == 1U);

    return 0;
}
