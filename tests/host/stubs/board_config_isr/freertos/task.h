#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t function,
    const char *name,
    uint32_t stack_bytes,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t core
);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
void vTaskDelete(TaskHandle_t task);
