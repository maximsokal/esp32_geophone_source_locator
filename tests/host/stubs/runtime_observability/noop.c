#include "runtime_observability.h"

void runtime_observability_beat(
    runtime_observability_task_t task,
    uint64_t progress
)
{
    (void)task;
    (void)progress;
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
