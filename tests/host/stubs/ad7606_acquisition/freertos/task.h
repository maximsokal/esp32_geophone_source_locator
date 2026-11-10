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
BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry,
    const char *name,
    uint32_t stack_depth,
    void *parameters,
    UBaseType_t priority,
    TaskHandle_t *created_task,
    BaseType_t core_id,
    UBaseType_t memory_caps);
void vTaskDelete(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher_priority_task_woken);
typedef enum {
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;
BaseType_t xTaskNotifyFromISR(
    TaskHandle_t task,
    uint32_t value,
    eNotifyAction action,
    BaseType_t *higher_priority_task_woken);
