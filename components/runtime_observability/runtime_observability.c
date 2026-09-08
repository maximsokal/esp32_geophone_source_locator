#include "runtime_observability.h"

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"


static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static runtime_observability_task_status_t s_tasks[RUNTIME_TASK_COUNT];


void runtime_observability_beat(
    runtime_observability_task_t task,
    uint64_t progress
)
{
    if (task < 0 || task >= RUNTIME_TASK_COUNT) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);

    runtime_observability_task_status_t *status = &s_tasks[task];
    status->observed = true;
    status->heartbeat++;
    status->progress = progress;
    status->last_heartbeat_ms = now_us > 0
        ? (uint64_t)(now_us / 1000LL)
        : 0U;

    portEXIT_CRITICAL(&s_lock);
}


void runtime_observability_get_snapshot(
    runtime_observability_snapshot_t *snapshot
)
{
    if (snapshot == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    /* Read the clock while the task table is locked. A beat that occurs
     * concurrently must never receive a timestamp newer than this snapshot. */
    const int64_t now_us = esp_timer_get_time();
    snapshot->now_ms = now_us > 0
        ? (uint64_t)(now_us / 1000LL)
        : 0U;
    for (size_t i = 0; i < RUNTIME_TASK_COUNT; ++i) {
        snapshot->tasks[i] = s_tasks[i];
    }
    portEXIT_CRITICAL(&s_lock);
}


const char *runtime_observability_task_name(
    runtime_observability_task_t task
)
{
    switch (task) {
        case RUNTIME_TASK_ACQUISITION:
            return "acquisition";
        case RUNTIME_TASK_RECORDER:
            return "recorder";
        case RUNTIME_TASK_UI:
            return "ui";
        case RUNTIME_TASK_KEYPAD:
            return "keypad";
        case RUNTIME_TASK_POWER_BUTTON:
            return "power_button";
        case RUNTIME_TASK_COUNT:
        default:
            return "unknown";
    }
}
