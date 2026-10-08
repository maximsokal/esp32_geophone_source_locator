#pragma once

#include <stdbool.h>
#include <stdint.h>


#define NRF_TOPOLOGY_LOG_HEARTBEAT_MS 60000U


typedef struct
{
    bool has_logged;
    uint32_t last_log_ms;
    uint32_t generation;
    bool tca_available;
    bool tca_fault;
    uint32_t assigned;
    uint32_t conflicts;
    int32_t main_state;
} nrf_topology_log_state_t;


static inline bool nrf_topology_log_should_emit(
    nrf_topology_log_state_t *state,
    uint32_t now_ms,
    uint32_t generation,
    bool tca_available,
    bool tca_fault,
    uint32_t assigned,
    uint32_t conflicts,
    int32_t main_state
)
{
    bool changed;
    bool heartbeat_due;

    changed = !state->has_logged;
    if (!changed) {
        changed =
            state->tca_available != tca_available ||
            state->tca_fault != tca_fault ||
            state->assigned != assigned ||
            state->conflicts != conflicts ||
            state->main_state != main_state;
    }

    heartbeat_due = state->has_logged &&
        (uint32_t)(now_ms - state->last_log_ms) >=
            NRF_TOPOLOGY_LOG_HEARTBEAT_MS;
    if (!changed && !heartbeat_due) {
        return false;
    }

    state->has_logged = true;
    state->last_log_ms = now_ms;
    state->generation = generation;
    state->tca_available = tca_available;
    state->tca_fault = tca_fault;
    state->assigned = assigned;
    state->conflicts = conflicts;
    state->main_state = main_state;
    return true;
}
