#pragma once

#include "freertos/FreeRTOS.h"

void vTaskDelay(TickType_t ticks_to_wait);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
