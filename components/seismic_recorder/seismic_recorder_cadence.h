#pragma once

#include <limits.h>
#include <string.h>

#include "seismic_recorder.h"

/*
 * Compute a recording-local delta from two cumulative acquisition snapshots.
 * Invalid, unavailable, reset or saturated evidence produces available=false.
 */
static inline void seismic_recorder_cadence_delta(
    const seismic_recorder_cadence_t *start,
    const seismic_recorder_cadence_t *end,
    seismic_recorder_cadence_t *result
)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    if (
        start == NULL || end == NULL ||
        !start->available || !end->available ||
        start->timer_ticks == UINT64_MAX ||
        start->conversion_attempts == UINT64_MAX ||
        start->collapsed_periods == UINT64_MAX ||
        end->timer_ticks == UINT64_MAX ||
        end->conversion_attempts == UINT64_MAX ||
        end->collapsed_periods == UINT64_MAX ||
        end->timer_ticks < start->timer_ticks ||
        end->conversion_attempts < start->conversion_attempts ||
        end->collapsed_periods < start->collapsed_periods
    ) return;

    const uint64_t timer_ticks = end->timer_ticks - start->timer_ticks;
    const uint64_t conversion_attempts = end->conversion_attempts - start->conversion_attempts;
    const uint64_t collapsed_periods = end->collapsed_periods - start->collapsed_periods;
    if (
        conversion_attempts > timer_ticks ||
        collapsed_periods != timer_ticks - conversion_attempts
    ) return;

    result->available = true;
    result->timer_ticks = timer_ticks;
    result->conversion_attempts = conversion_attempts;
    result->collapsed_periods = collapsed_periods;
}
