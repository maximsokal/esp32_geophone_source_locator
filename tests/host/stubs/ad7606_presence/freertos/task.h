#pragma once

#include "freertos/FreeRTOS.h"

void vTaskDelay(TickType_t ticks_to_delay);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
