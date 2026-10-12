#pragma once

#include "freertos/FreeRTOS.h"

TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
