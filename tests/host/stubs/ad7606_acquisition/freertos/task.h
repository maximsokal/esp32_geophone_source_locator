#pragma once

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry,
    const char *name,
    uint32_t stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
