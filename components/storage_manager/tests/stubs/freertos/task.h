#pragma once

#include "freertos/FreeRTOS.h"

typedef enum
{
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid
} eTaskState;

typedef struct
{
    TaskHandle_t xHandle;
    const char *pcTaskName;
    UBaseType_t uxCurrentPriority;
    UBaseType_t uxBasePriority;
} TaskStatus_t;

void vTaskDelay(unsigned long ticks);
void vTaskGetInfo(
    TaskHandle_t task,
    TaskStatus_t *status,
    BaseType_t get_free_stack_space,
    eTaskState state
);
