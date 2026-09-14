#include <assert.h>

#include "nrf_node_state.h"

static void test_boot_ready_main_disconnect_reconnect_reset(void)
{
    nrf_node_state_t state;

    nrf_node_state_init(&state);
    assert(state.role == NRF_NODE_ROLE_UNASSIGNED);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_BOOTING);

    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_MAIN) ==
           NRF_NODE_STATE_ERR_NOT_READY);
    assert(state.role == NRF_NODE_ROLE_UNASSIGNED);

    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_READY);

    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_MAIN) ==
           NRF_NODE_STATE_OK);
    assert(state.role == NRF_NODE_ROLE_MAIN);

    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_MAIN) ==
           NRF_NODE_STATE_OK);
    assert(state.role == NRF_NODE_ROLE_MAIN);

    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_REMOTE) ==
           NRF_NODE_STATE_ERR_ROLE_CONFLICT);
    assert(state.role == NRF_NODE_ROLE_MAIN);

    assert(nrf_node_state_mark_unavailable(&state) == NRF_NODE_STATE_OK);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_UNAVAILABLE);
    assert(state.role == NRF_NODE_ROLE_MAIN);

    assert(nrf_node_state_mark_available(&state) == NRF_NODE_STATE_OK);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_READY);
    assert(state.role == NRF_NODE_ROLE_MAIN);

    nrf_node_state_reset(&state);
    assert(state.role == NRF_NODE_ROLE_UNASSIGNED);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_BOOTING);
}

static void test_remote_role_is_runtime_assignable(void)
{
    nrf_node_state_t state;

    nrf_node_state_init(&state);
    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);
    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_REMOTE) ==
           NRF_NODE_STATE_OK);
    assert(state.role == NRF_NODE_ROLE_REMOTE);

    assert(nrf_node_state_mark_unavailable(&state) == NRF_NODE_STATE_OK);
    assert(nrf_node_state_mark_unavailable(&state) == NRF_NODE_STATE_OK);
    assert(state.role == NRF_NODE_ROLE_REMOTE);

    assert(nrf_node_state_mark_available(&state) == NRF_NODE_STATE_OK);
    assert(nrf_node_state_mark_available(&state) == NRF_NODE_STATE_OK);
    assert(state.role == NRF_NODE_ROLE_REMOTE);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_READY);
}

static void test_invalid_inputs_do_not_mutate_state(void)
{
    nrf_node_state_t state;

    nrf_node_state_init(&state);
    assert(nrf_node_state_mark_ready(&state) == NRF_NODE_STATE_OK);

    assert(nrf_node_state_assign_role(&state, NRF_NODE_ROLE_UNASSIGNED) ==
           NRF_NODE_STATE_ERR_ARGUMENT);
    assert(state.role == NRF_NODE_ROLE_UNASSIGNED);
    assert(state.lifecycle == NRF_NODE_LIFECYCLE_READY);

    assert(nrf_node_state_mark_ready(0) == NRF_NODE_STATE_ERR_ARGUMENT);
    assert(nrf_node_state_assign_role(0, NRF_NODE_ROLE_MAIN) ==
           NRF_NODE_STATE_ERR_ARGUMENT);
    assert(nrf_node_state_mark_unavailable(0) == NRF_NODE_STATE_ERR_ARGUMENT);
    assert(nrf_node_state_mark_available(0) == NRF_NODE_STATE_ERR_ARGUMENT);

    nrf_node_state_init(0);
    nrf_node_state_reset(0);
}

int main(void)
{
    test_boot_ready_main_disconnect_reconnect_reset();
    test_remote_role_is_runtime_assignable();
    test_invalid_inputs_do_not_mutate_state();
    return 0;
}
