#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"


typedef void (*TaskFunction_t)(void *argument);

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task_code,
    const char *name,
    uint32_t stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id,
    uint32_t memory_caps
);
