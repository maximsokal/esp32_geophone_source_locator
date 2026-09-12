#pragma once
#include <stdint.h>
typedef enum { RUNTIME_TASK_ACQUISITION = 0, RUNTIME_TASK_RECORDER, RUNTIME_TASK_UI, RUNTIME_TASK_COUNT } runtime_observability_task_t;
void runtime_observability_beat(runtime_observability_task_t task, uint64_t progress);
