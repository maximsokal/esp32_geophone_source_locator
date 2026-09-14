#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_bootstrap.h"

static bool s_registered;
static uint8_t s_address;
static int s_readdress_result;
static bool s_readdress_leaves_discovery;
static unsigned s_readdress_calls;
static unsigned s_reboot_calls;

static int fake_readdress(void *context, uint8_t address)
{
    (void)context;
    ++s_readdress_calls;
    if (s_readdress_result == 0) {
        s_registered = true;
        s_address = address;
        return 0;
    }
    if (s_readdress_leaves_discovery) {
        s_registered = true;
        s_address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
    }
    else {
        s_registered = false;
        s_address = 0U;
    }
    return s_readdress_result;
}

static bool fake_registered(void *context)
{
    (void)context;
    return s_registered;
}

static uint8_t fake_address(void *context)
{
    (void)context;
    return s_registered ? s_address : 0U;
}

static void fake_reboot(void *context)
{
    (void)context;
    ++s_reboot_calls;
}

static void reset_transport(void)
{
    s_registered = true;
    s_address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
    s_readdress_result = 0;
    s_readdress_leaves_discovery = true;
    s_readdress_calls = 0U;
    s_reboot_calls = 0U;
}

static nrf_upstream_assign_t make_assign(uint32_t request_id, uint8_t role, uint8_t node_id, uint8_t channel)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = request_id;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 0x10U;
    assign.hardware_id[1] = 0x20U;
    assign.hardware_id[2] = 0x30U;
    assign.hardware_id[3] = 0x40U;
    assign.role = role;
    assign.node_id = node_id;
    assign.mux_channel = channel;
    return assign;
}

static void init_bootstrap(nrf_bootstrap_t *bootstrap, nrf_node_state_t *node_state)
{
    const uint8_t hardware_id[] = {0x10U, 0x20U, 0x30U, 0x40U};
    const nrf_bootstrap_ops_t ops = {
        .readdress = fake_readdress,
        .target_is_registered = fake_registered,
        .target_address = fake_address,
        .cold_reboot = fake_reboot,
        .context = NULL,
    };
    nrf_node_state_init(node_state);
    assert(nrf_node_state_mark_ready(node_state) == NRF_NODE_STATE_OK);
    assert(nrf_bootstrap_init(bootstrap, node_state, hardware_id, sizeof(hardware_id), &ops) == NRF_UPSTREAM_RESULT_OK);
}

static void test_success_status_and_idempotency(void)
{
    nrf_bootstrap_t bootstrap;
    nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_upstream_assign_t assign = make_assign(11U, NRF_UPSTREAM_ROLE_REMOTE, 4U, 3U);

    reset_transport();
    init_bootstrap(&bootstrap, &node_state);
    assert(nrf_bootstrap_make_status(&bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.request_id == 0U && status.role == NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    assert(status.flags == NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED);

    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    assert(s_readdress_calls == 1U && s_address == NRF_UPSTREAM_ADDRESS_REMOTE);
    assert(node_state.role == NRF_NODE_ROLE_REMOTE);
    assert(nrf_bootstrap_make_status(&bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.request_id == 11U && status.role == NRF_UPSTREAM_ROLE_REMOTE);
    assert(status.node_id == 4U && status.mux_channel == 3U);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_REMOTE);
    assert(status.result == NRF_UPSTREAM_RESULT_OK);
    assert(status.flags == (NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED));

    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    assert(s_readdress_calls == 1U);

    assign.request_id = 12U;
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    assert(s_readdress_calls == 1U);
    assert(nrf_bootstrap_make_status(&bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.request_id == 12U);
}

static void test_request_conflicts_and_hardware_mismatch(void)
{
    nrf_bootstrap_t bootstrap;
    nrf_node_state_t node_state;
    nrf_upstream_assign_t assign = make_assign(21U, NRF_UPSTREAM_ROLE_MAIN, 1U, NRF_UPSTREAM_MUX_CHANNEL_NONE);
    nrf_upstream_assign_t other = assign;

    reset_transport();
    init_bootstrap(&bootstrap, &node_state);
    s_readdress_result = -1;
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_READDRESS_FAILED);
    other.node_id = 2U;
    assert(nrf_bootstrap_process_assign(&bootstrap, &other) == NRF_UPSTREAM_RESULT_REQUEST_CONFLICT);

    other = make_assign(22U, NRF_UPSTREAM_ROLE_MAIN, 1U, NRF_UPSTREAM_MUX_CHANNEL_NONE);
    other.hardware_id[0] ^= 1U;
    assert(nrf_bootstrap_process_assign(&bootstrap, &other) == NRF_UPSTREAM_RESULT_HW_ID_MISMATCH);
}

static void test_failed_readdress_can_retry_same_request(void)
{
    nrf_bootstrap_t bootstrap;
    nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_upstream_assign_t assign = make_assign(31U, NRF_UPSTREAM_ROLE_MAIN, 1U, NRF_UPSTREAM_MUX_CHANNEL_NONE);

    reset_transport();
    init_bootstrap(&bootstrap, &node_state);
    s_readdress_result = -5;
    s_readdress_leaves_discovery = true;
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_READDRESS_FAILED);
    assert(node_state.role == NRF_NODE_ROLE_UNASSIGNED);
    assert(s_registered && s_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    assert(nrf_bootstrap_make_status(&bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.result == NRF_UPSTREAM_RESULT_READDRESS_FAILED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    s_readdress_result = 0;
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    assert(s_readdress_calls == 2U);
    assert(node_state.role == NRF_NODE_ROLE_MAIN);
    assert(s_address == NRF_UPSTREAM_ADDRESS_MAIN);
}

static void test_unrecoverable_readdress_requests_cold_reboot(void)
{
    nrf_bootstrap_t bootstrap;
    nrf_node_state_t node_state;
    nrf_upstream_assign_t assign = make_assign(41U, NRF_UPSTREAM_ROLE_REMOTE, 2U, 0U);

    reset_transport();
    init_bootstrap(&bootstrap, &node_state);
    s_readdress_result = -5;
    s_readdress_leaves_discovery = false;
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
    assert(s_reboot_calls == 1U);
    assert(!s_registered);
    assert(node_state.role == NRF_NODE_ROLE_UNASSIGNED);
}

static void test_different_assignment_after_success_conflicts(void)
{
    nrf_bootstrap_t bootstrap;
    nrf_node_state_t node_state;
    nrf_upstream_assign_t assign = make_assign(51U, NRF_UPSTREAM_ROLE_REMOTE, 2U, 1U);
    nrf_upstream_assign_t other = make_assign(52U, NRF_UPSTREAM_ROLE_REMOTE, 3U, 1U);

    reset_transport();
    init_bootstrap(&bootstrap, &node_state);
    assert(nrf_bootstrap_process_assign(&bootstrap, &assign) == NRF_UPSTREAM_RESULT_OK);
    assert(nrf_bootstrap_process_assign(&bootstrap, &other) == NRF_UPSTREAM_RESULT_ROLE_CONFLICT);
    assert(s_readdress_calls == 1U);
}

int main(void)
{
    test_success_status_and_idempotency();
    test_request_conflicts_and_hardware_mismatch();
    test_failed_readdress_can_retry_same_request();
    test_unrecoverable_readdress_requests_cold_reboot();
    test_different_assignment_after_success_conflicts();
    return 0;
}
