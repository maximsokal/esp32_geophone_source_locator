#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>


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

typedef enum {
    NRF_TOPOLOGY_TCA_LOG_NONE = 0,
    NRF_TOPOLOGY_TCA_LOG_SET,
    NRF_TOPOLOGY_TCA_LOG_CLEARED,
    NRF_TOPOLOGY_TCA_LOG_BUSY,
} nrf_topology_tca_log_event_t;

typedef struct {
    bool fault_latched;
    bool mutex_busy_reported;
    uint32_t latched_count;
    uint32_t auto_cleared_count;
    char last_reason[32];
} nrf_topology_tca_log_state_t;

static inline void nrf_topology_tca_log_set_reason(
    nrf_topology_tca_log_state_t *state,
    const char *reason)
{
    size_t index = 0U;
    if (reason != NULL) {
        while (index + 1U < sizeof(state->last_reason) && reason[index] != '\0') {
            state->last_reason[index] = reason[index];
            ++index;
        }
    }
    state->last_reason[index] = '\0';
}

static inline nrf_topology_tca_log_event_t nrf_topology_tca_log_update(
    nrf_topology_tca_log_state_t *state,
    bool fault_latched,
    bool mutex_busy,
    const char *reason)
{
    if (state == NULL) {
        return NRF_TOPOLOGY_TCA_LOG_NONE;
    }
    if (mutex_busy) {
        if (state->mutex_busy_reported) {
            return NRF_TOPOLOGY_TCA_LOG_NONE;
        }
        state->mutex_busy_reported = true;
        nrf_topology_tca_log_set_reason(state, "mutex_busy");
        return NRF_TOPOLOGY_TCA_LOG_BUSY;
    }
    state->mutex_busy_reported = false;
    if (fault_latched && !state->fault_latched) {
        state->fault_latched = true;
        if (state->latched_count < UINT32_MAX) ++state->latched_count;
        nrf_topology_tca_log_set_reason(state, reason);
        return NRF_TOPOLOGY_TCA_LOG_SET;
    }
    if (!fault_latched && state->fault_latched) {
        state->fault_latched = false;
        if (state->auto_cleared_count < UINT32_MAX) ++state->auto_cleared_count;
        nrf_topology_tca_log_set_reason(state, reason);
        return NRF_TOPOLOGY_TCA_LOG_CLEARED;
    }
    return NRF_TOPOLOGY_TCA_LOG_NONE;
}


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
