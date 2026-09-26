#pragma once

typedef unsigned TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)~0U)
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
