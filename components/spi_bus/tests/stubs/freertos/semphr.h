#pragma once

#include "freertos/FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
