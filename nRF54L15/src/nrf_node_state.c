#include "nrf_node_state.h"

static int nrf_node_role_is_assignable(nrf_node_role_t role)
{
    return role == NRF_NODE_ROLE_MAIN || role == NRF_NODE_ROLE_REMOTE;
}

void nrf_node_state_init(nrf_node_state_t *state)
{
    if (state == 0) {
        return;
    }

    state->role = NRF_NODE_ROLE_UNASSIGNED;
    state->lifecycle = NRF_NODE_LIFECYCLE_BOOTING;
}

void nrf_node_state_reset(nrf_node_state_t *state)
{
    nrf_node_state_init(state);
}

nrf_node_state_result_t nrf_node_state_mark_ready(nrf_node_state_t *state)
{
    if (state == 0) {
        return NRF_NODE_STATE_ERR_ARGUMENT;
    }

    state->lifecycle = NRF_NODE_LIFECYCLE_READY;
    return NRF_NODE_STATE_OK;
}

nrf_node_state_result_t nrf_node_state_assign_role(
    nrf_node_state_t *state,
    nrf_node_role_t role)
{
    if (state == 0 || !nrf_node_role_is_assignable(role)) {
        return NRF_NODE_STATE_ERR_ARGUMENT;
    }

    if (state->lifecycle != NRF_NODE_LIFECYCLE_READY) {
        return NRF_NODE_STATE_ERR_NOT_READY;
    }

    if (state->role == NRF_NODE_ROLE_UNASSIGNED || state->role == role) {
        state->role = role;
        return NRF_NODE_STATE_OK;
    }

    return NRF_NODE_STATE_ERR_ROLE_CONFLICT;
}

nrf_node_state_result_t nrf_node_state_mark_unavailable(nrf_node_state_t *state)
{
    if (state == 0) {
        return NRF_NODE_STATE_ERR_ARGUMENT;
    }

    if (state->lifecycle == NRF_NODE_LIFECYCLE_BOOTING) {
        return NRF_NODE_STATE_ERR_NOT_READY;
    }

    state->lifecycle = NRF_NODE_LIFECYCLE_UNAVAILABLE;
    return NRF_NODE_STATE_OK;
}

nrf_node_state_result_t nrf_node_state_mark_available(nrf_node_state_t *state)
{
    if (state == 0) {
        return NRF_NODE_STATE_ERR_ARGUMENT;
    }

    if (state->lifecycle == NRF_NODE_LIFECYCLE_BOOTING) {
        return NRF_NODE_STATE_ERR_NOT_READY;
    }

    state->lifecycle = NRF_NODE_LIFECYCLE_READY;
    return NRF_NODE_STATE_OK;
}
