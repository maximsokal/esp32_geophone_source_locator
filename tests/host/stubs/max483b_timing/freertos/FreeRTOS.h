#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

#ifndef CONFIG_FREERTOS_HZ
#error "CONFIG_FREERTOS_HZ must match the checked-in ESP-IDF configuration"
#endif

#define configTICK_RATE_HZ CONFIG_FREERTOS_HZ
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000U))
