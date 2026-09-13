#pragma once

#include <stdint.h>

#define RUNTIME_TASK_UI 1

void runtime_observability_beat(int task, uint64_t progress);
