#pragma once

#include "freertos/FreeRTOS.h"

typedef struct test_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);

BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t ticks_to_wait
);

BaseType_t xSemaphoreGive(
    SemaphoreHandle_t semaphore
);
