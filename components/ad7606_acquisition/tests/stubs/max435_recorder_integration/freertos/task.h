#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *argument);

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher_priority_task_woken);
typedef enum {
    eNoAction = 0,
    eSetBits = 1,
    eIncrement = 2,
    eSetValueWithOverwrite = 3,
    eSetValueWithoutOverwrite = 4,
} eNotifyAction;
BaseType_t xTaskNotifyFromISR(
    TaskHandle_t task,
    uint32_t value,
    eNotifyAction action,
    BaseType_t *higher_priority_task_woken);
void vTaskDelete(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id,
    unsigned memory_caps
);
void vTaskDeleteWithCaps(TaskHandle_t task);
