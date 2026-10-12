#pragma once

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
