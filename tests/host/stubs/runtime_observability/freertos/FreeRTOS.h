#pragma once

#include <stdint.h>


typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;

typedef struct { int unused; } portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY ((uint32_t)~0U)
#define pdMS_TO_TICKS(milliseconds) ((uint32_t)(milliseconds))
#define tskNO_AFFINITY (-1)
