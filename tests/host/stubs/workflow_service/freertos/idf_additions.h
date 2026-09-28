#pragma once

#include <stdint.h>
#include "freertos/task.h"

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    void (*task)(void *), const char *name, uint32_t stack_bytes, void *argument,
    unsigned priority, TaskHandle_t *task_out, int core_id, unsigned caps);
