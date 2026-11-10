#pragma once

#include <stddef.h>

#define portMAX_DELAY ((unsigned long)-1L)
#define pdMS_TO_TICKS(ms) ((unsigned long)((ms) / 10U))
#define configUSE_TRACE_FACILITY 1

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;

#define pdTRUE 1
#define pdFALSE 0
