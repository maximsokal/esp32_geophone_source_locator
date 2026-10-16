#include "runtime_observability.h"

void runtime_observability_beat(
    runtime_observability_task_t task,
    uint64_t progress
)
{
    (void)task;
    (void)progress;
}

void runtime_observability_set_health_phase(
    runtime_observability_task_t task,
    runtime_health_phase_t phase
)
{
    (void)task;
    (void)phase;
}

runtime_health_result_t runtime_observability_get_health_result(
    runtime_observability_task_t task
)
{
    (void)task;
    return RUNTIME_HEALTH_OK;
}

bool runtime_observability_start(void)
{
    return true;
}

void runtime_observability_stop(void)
{
}

void runtime_observability_log_snapshot(const char *stage)
{
    (void)stage;
}

void runtime_observability_log_memory(const char *stage)
{
    (void)stage;
}

void runtime_observability_request_snapshot(const char *stage)
{
    (void)stage;
}
