#pragma once

#include <stdint.h>

/* FreeRTOS rounds milliseconds down; retain that policy but never return 0
 * ticks for a positive wait that must actually wait. */
static inline uint32_t app_ms_to_ticks_min_one(uint32_t milliseconds, uint32_t tick_rate_hz)
{
    if (milliseconds == 0U || tick_rate_hz == 0U) {
        return 0U;
    }
    const uint64_t ticks = ((uint64_t)milliseconds * tick_rate_hz) / 1000U;
    return ticks == 0U ? 1U : (ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks);
}
