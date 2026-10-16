#pragma once

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

void vTaskDelay(TickType_t ticks_to_wait);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
