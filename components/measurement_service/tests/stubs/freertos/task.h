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
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);

/* Host tests do not model or claim a real FreeRTOS task stack. */
static inline UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task)
{
    (void)task;
    return 0U;
}
