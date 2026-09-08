#pragma once

#include <stdbool.h>

typedef unsigned BaseType_t;
typedef unsigned TickType_t;
typedef unsigned EventBits_t;
typedef void *EventGroupHandle_t;
typedef void *TaskHandle_t;

#define pdTRUE       1U
#define pdFALSE      0U
#define pdPASS       1U
#define portMAX_DELAY ((TickType_t)~0U)
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))
#define BIT0         (1U << 0)
