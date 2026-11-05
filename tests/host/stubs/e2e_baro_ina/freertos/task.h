#pragma once
#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    uint32_t stack_depth,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
