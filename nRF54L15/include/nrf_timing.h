#ifndef NRF_TIMING_H
#define NRF_TIMING_H

#include <stdint.h>

static inline uint64_t nrf_timing_ms_until_u32(
    uint64_t now_ms,
    uint32_t started_ms,
    uint32_t duration_ms)
{
    const uint32_t elapsed_ms = (uint32_t)now_ms - started_ms;

    return elapsed_ms >= duration_ms ? 0U : duration_ms - elapsed_ms;
}

static inline uint64_t nrf_timing_next_periodic_deadline_ms(
    uint64_t scheduled_ms,
    uint64_t now_ms,
    uint32_t period_ms)
{
    uint64_t periods;

    if (period_ms == 0U) {
        return now_ms;
    }
    if (scheduled_ms > now_ms) {
        return scheduled_ms;
    }

    periods = (now_ms - scheduled_ms) / period_ms;
    if (periods == UINT64_MAX ||
        ++periods > (UINT64_MAX - scheduled_ms) / period_ms) {
        return UINT64_MAX;
    }
    return scheduled_ms + periods * period_ms;
}

#endif
