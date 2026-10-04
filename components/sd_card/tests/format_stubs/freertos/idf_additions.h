#pragma once

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    unsigned stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id,
    UBaseType_t memory_caps
);
