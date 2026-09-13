#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"


typedef struct {
    TaskHandle_t xHandle;
    const char *pcTaskName;
    UBaseType_t xTaskNumber;
    UBaseType_t eCurrentState;
    UBaseType_t uxCurrentPriority;
    UBaseType_t uxBasePriority;
    uint64_t ulRunTimeCounter;
    UBaseType_t usStackHighWaterMark;
    BaseType_t xCoreID;
} TaskStatus_t;

UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(
    TaskStatus_t *task_status_array,
    UBaseType_t array_size,
    configRUN_TIME_COUNTER_TYPE *total_runtime
);
uint64_t ulTaskGetIdleRunTimeCounterForCore(BaseType_t core);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, uint32_t ticks_to_wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);
