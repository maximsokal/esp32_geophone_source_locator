#pragma once

#include <stddef.h>
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

/* Compare FreeRTOS ticks only; esp_timer_get_time() values are microseconds.
 * Skip expired periodic slots before vTaskDelayUntil; wrap-safe when the
 * period and lateness stay below half the uint32_t range.
 */
static inline uint32_t app_tick_skip_missed_periods(
    uint32_t *last_wake_tick,
    uint32_t now_tick,
    uint32_t period_ticks)
{
    if (last_wake_tick == NULL || period_ticks == 0U || period_ticks >= UINT32_C(0x80000000)) {
        return 0U;
    }

    const uint32_t next_deadline_tick = *last_wake_tick + period_ticks;
    const uint32_t elapsed_after_deadline = now_tick - next_deadline_tick;
    if (elapsed_after_deadline == 0U || elapsed_after_deadline >= UINT32_C(0x80000000)) {
        return 0U;
    }

    const uint32_t missed_periods = (elapsed_after_deadline - 1U) / period_ticks + 1U;
    *last_wake_tick += missed_periods * period_ticks;
    return missed_periods;
}
