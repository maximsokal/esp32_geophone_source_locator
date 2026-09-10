#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *argument);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned stack_size, void *argument, unsigned priority, TaskHandle_t *task_handle, int core_id);
void vTaskDelay(TickType_t ticks);
