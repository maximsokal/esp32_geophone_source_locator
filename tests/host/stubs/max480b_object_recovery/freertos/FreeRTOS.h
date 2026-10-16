#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 100U
#endif

#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
