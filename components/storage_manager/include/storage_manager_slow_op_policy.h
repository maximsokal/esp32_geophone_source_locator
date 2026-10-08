#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define STORAGE_MANAGER_SLOW_OP_THRESHOLD_MS  500U
#define STORAGE_MANAGER_SLOW_OP_INTERVAL_MS  5000U
#define STORAGE_MANAGER_SLOW_OP_SOURCE_COUNT  2U


typedef struct
{
    uint32_t last_warn_ms[STORAGE_MANAGER_SLOW_OP_SOURCE_COUNT];
    bool warned[STORAGE_MANAGER_SLOW_OP_SOURCE_COUNT];

} storage_manager_slow_op_state_t;


/* Round up so any duration above 500 ms remains above the strict threshold. */
static inline uint32_t storage_manager_slow_op_elapsed_ms(int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0U;
    }

    const uint64_t elapsed_us_unsigned = (uint64_t)elapsed_us;
    const uint64_t elapsed_ms =
        elapsed_us_unsigned / 1000U +
        (elapsed_us_unsigned % 1000U != 0U ? 1U : 0U);

    return elapsed_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)elapsed_ms;
}


/* Durations equal to 500 ms are not slow; only values strictly above it warn. */
static inline bool storage_manager_slow_op_should_warn(
    storage_manager_slow_op_state_t *state,
    size_t source_id,
    uint32_t now_ms,
    uint32_t elapsed_ms
)
{
    if (
        state == NULL ||
        source_id >= STORAGE_MANAGER_SLOW_OP_SOURCE_COUNT ||
        elapsed_ms <= STORAGE_MANAGER_SLOW_OP_THRESHOLD_MS
    ) {
        return false;
    }

    if (
        !state->warned[source_id] ||
        (uint32_t)(now_ms - state->last_warn_ms[source_id]) >=
            STORAGE_MANAGER_SLOW_OP_INTERVAL_MS
    ) {
        state->last_warn_ms[source_id] = now_ms;
        state->warned[source_id] = true;
        return true;
    }

    return false;
}
