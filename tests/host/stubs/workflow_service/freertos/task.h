#pragma once

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *last_wake, TickType_t period);
void vTaskDelete(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);
