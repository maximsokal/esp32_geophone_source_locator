#pragma once

#include <stddef.h>

typedef int BaseType_t;
typedef unsigned long TickType_t;
typedef unsigned UBaseType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY ((TickType_t)-1L)
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
