#pragma once

#include "freertos/FreeRTOS.h"

static inline TickType_t i2c_timeout_remaining_ticks(
    TickType_t started, TickType_t budget, TickType_t now)
{
    if (budget == portMAX_DELAY) return portMAX_DELAY;
    const TickType_t elapsed = now - started;
    return elapsed >= budget ? 0U : budget - elapsed;
}
