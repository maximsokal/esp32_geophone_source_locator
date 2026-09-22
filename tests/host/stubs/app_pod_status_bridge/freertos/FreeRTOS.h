#pragma once

#include <stddef.h>

typedef int BaseType_t;
typedef unsigned long TickType_t;
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(milliseconds) (milliseconds)
#define portMAX_DELAY ((TickType_t)-1L)
#define portMUX_INITIALIZER_UNLOCKED 0

#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
