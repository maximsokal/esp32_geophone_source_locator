#include "nrf_bootstrap.h"

#include <string.h>

static bool assignment_payload_equal(
    const nrf_upstream_assign_t *left,
    const nrf_upstream_assign_t *right)
{
    return left->hardware_id_length == right->hardware_id_length &&
           memcmp(left->hardware_id, right->hardware_id, NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) == 0 &&
           left->role == right->role &&
           left->node_id == right->node_id &&
           left->mux_channel == right->mux_channel;
}

static nrf_upstream_result_t validate_hardware_identity(
    const nrf_bootstrap_t *bootstrap,
    const nrf_upstream_assign_t *assign)
{
    if (assign->hardware_id_length != bootstrap->hardware_id_length ||
        memcmp(assign->hardware_id, bootstrap->hardware_id, bootstrap->hardware_id_length) != 0) {
        return NRF_UPSTREAM_RESULT_HW_ID_MISMATCH;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

static uint8_t lifecycle_to_wire(nrf_node_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case NRF_NODE_LIFECYCLE_BOOTING:
        return NRF_UPSTREAM_LIFECYCLE_BOOTING;
    case NRF_NODE_LIFECYCLE_READY:
        return NRF_UPSTREAM_LIFECYCLE_READY;
    case NRF_NODE_LIFECYCLE_UNAVAILABLE:
        return NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE;
    default:
        return NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE;
    }
}

static uint8_t role_to_wire(nrf_node_role_t role)
{
    switch (role) {
    case NRF_NODE_ROLE_MAIN:
        return NRF_UPSTREAM_ROLE_MAIN;
    case NRF_NODE_ROLE_REMOTE:
        return NRF_UPSTREAM_ROLE_REMOTE;
    case NRF_NODE_ROLE_UNASSIGNED:
    default:
        return NRF_UPSTREAM_ROLE_UNASSIGNED;
    }
}

static nrf_node_role_t role_from_wire(uint8_t role)
{
    return role == NRF_UPSTREAM_ROLE_MAIN ? NRF_NODE_ROLE_MAIN : NRF_NODE_ROLE_REMOTE;
}

static void set_result(
    nrf_bootstrap_t *bootstrap,
    uint32_t request_id,
    nrf_upstream_result_t result)
{
    bootstrap->status_request_id = request_id;
    bootstrap->result = result;
}

static nrf_upstream_result_t fail_or_reboot_after_readdress(
    nrf_bootstrap_t *bootstrap,
    uint32_t request_id)
{
    if (bootstrap->ops.target_is_registered(bootstrap->ops.context) &&
        bootstrap->ops.target_address(bootstrap->ops.context) == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        set_result(bootstrap, request_id, NRF_UPSTREAM_RESULT_READDRESS_FAILED);
        return NRF_UPSTREAM_RESULT_READDRESS_FAILED;
    }

    set_result(bootstrap, request_id, NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
    bootstrap->ops.cold_reboot(bootstrap->ops.context);
    return NRF_UPSTREAM_RESULT_INTERNAL_ERROR;
}

nrf_upstream_result_t nrf_bootstrap_init(
    nrf_bootstrap_t *bootstrap,
    nrf_node_state_t *node_state,
    const uint8_t *hardware_id,
    size_t hardware_id_length,
    const nrf_bootstrap_ops_t *ops)
{
    if (bootstrap == NULL || node_state == NULL || hardware_id == NULL || ops == NULL ||
        hardware_id_length == 0U || hardware_id_length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX ||
        ops->readdress == NULL || ops->target_is_registered == NULL ||
        ops->target_address == NULL || ops->cold_reboot == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (node_state->lifecycle != NRF_NODE_LIFECYCLE_READY ||
        node_state->role != NRF_NODE_ROLE_UNASSIGNED) {
        return NRF_UPSTREAM_RESULT_BUSY;
    }

    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->node_state = node_state;
    bootstrap->ops = *ops;
    bootstrap->hardware_id_length = (uint8_t)hardware_id_length;
    memcpy(bootstrap->hardware_id, hardware_id, hardware_id_length);
    bootstrap->node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    bootstrap->mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    bootstrap->result = NRF_UPSTREAM_RESULT_OK;
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_bootstrap_process_assign(
    nrf_bootstrap_t *bootstrap,
    const nrf_upstream_assign_t *assign)
{
    nrf_upstream_result_t result;
    nrf_node_state_result_t node_result;
    uint8_t target_address;

    if (bootstrap == NULL || assign == NULL || bootstrap->node_state == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    result = nrf_upstream_protocol_validate_assign(assign);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        set_result(bootstrap, assign->request_id, result);
        return result;
    }

    result = validate_hardware_identity(bootstrap, assign);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        set_result(bootstrap, assign->request_id, result);
        return result;
    }

    if (bootstrap->request_history_valid &&
        assign->request_id == bootstrap->request_history_id) {
        if (!assignment_payload_equal(assign, &bootstrap->request_history_assign)) {
            set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_REQUEST_CONFLICT);
            return NRF_UPSTREAM_RESULT_REQUEST_CONFLICT;
        }
        if (bootstrap->request_history_result == NRF_UPSTREAM_RESULT_OK) {
            set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_OK);
            return NRF_UPSTREAM_RESULT_OK;
        }
        if (bootstrap->request_history_result == NRF_UPSTREAM_RESULT_PENDING) {
            set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_BUSY);
            return NRF_UPSTREAM_RESULT_BUSY;
        }
    }

    if (bootstrap->assigned) {
        if (assign->role == role_to_wire(bootstrap->node_state->role) &&
            assign->node_id == bootstrap->node_id &&
            assign->mux_channel == bootstrap->mux_channel) {
            bootstrap->request_history_valid = true;
            bootstrap->request_history_id = assign->request_id;
            bootstrap->request_history_assign = *assign;
            bootstrap->request_history_result = NRF_UPSTREAM_RESULT_OK;
            set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_OK);
            return NRF_UPSTREAM_RESULT_OK;
        }
        set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_ROLE_CONFLICT);
        return NRF_UPSTREAM_RESULT_ROLE_CONFLICT;
    }

    if (bootstrap->node_state->lifecycle != NRF_NODE_LIFECYCLE_READY) {
        set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_BUSY);
        return NRF_UPSTREAM_RESULT_BUSY;
    }
    if (bootstrap->node_state->role != NRF_NODE_ROLE_UNASSIGNED) {
        set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_ROLE_CONFLICT);
        return NRF_UPSTREAM_RESULT_ROLE_CONFLICT;
    }

    target_address = nrf_upstream_protocol_address_for_role(assign->role);
    if (target_address == 0U) {
        set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_ROLE_INVALID);
        return NRF_UPSTREAM_RESULT_ROLE_INVALID;
    }

    bootstrap->request_history_valid = true;
    bootstrap->request_history_id = assign->request_id;
    bootstrap->request_history_assign = *assign;
    bootstrap->request_history_result = NRF_UPSTREAM_RESULT_PENDING;
    set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_PENDING);

    if (bootstrap->ops.readdress(bootstrap->ops.context, target_address) != 0 ||
        !bootstrap->ops.target_is_registered(bootstrap->ops.context) ||
        bootstrap->ops.target_address(bootstrap->ops.context) != target_address) {
        result = fail_or_reboot_after_readdress(bootstrap, assign->request_id);
        bootstrap->request_history_result = result;
        return result;
    }

    node_result = nrf_node_state_assign_role(bootstrap->node_state, role_from_wire(assign->role));
    if (node_result != NRF_NODE_STATE_OK) {
        (void)bootstrap->ops.readdress(bootstrap->ops.context, NRF_UPSTREAM_ADDRESS_DISCOVERY);
        if (bootstrap->ops.target_is_registered(bootstrap->ops.context) &&
            bootstrap->ops.target_address(bootstrap->ops.context) == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
            result = node_result == NRF_NODE_STATE_ERR_ROLE_CONFLICT
                ? NRF_UPSTREAM_RESULT_ROLE_CONFLICT
                : NRF_UPSTREAM_RESULT_INTERNAL_ERROR;
            set_result(bootstrap, assign->request_id, result);
            bootstrap->request_history_result = result;
            return result;
        }
        bootstrap->request_history_result = NRF_UPSTREAM_RESULT_INTERNAL_ERROR;
        set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
        bootstrap->ops.cold_reboot(bootstrap->ops.context);
        return NRF_UPSTREAM_RESULT_INTERNAL_ERROR;
    }

    bootstrap->assigned = true;
    bootstrap->node_id = assign->node_id;
    bootstrap->mux_channel = assign->mux_channel;
    bootstrap->request_history_result = NRF_UPSTREAM_RESULT_OK;
    set_result(bootstrap, assign->request_id, NRF_UPSTREAM_RESULT_OK);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_bootstrap_make_status(
    const nrf_bootstrap_t *bootstrap,
    nrf_upstream_status_t *status)
{
    if (bootstrap == NULL || status == NULL || bootstrap->node_state == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    memset(status, 0, sizeof(*status));
    status->request_id = bootstrap->status_request_id;
    status->hardware_id_length = bootstrap->hardware_id_length;
    memcpy(status->hardware_id, bootstrap->hardware_id, bootstrap->hardware_id_length);
    status->role = role_to_wire(bootstrap->node_state->role);
    status->node_id = bootstrap->assigned ? bootstrap->node_id : NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    status->mux_channel = bootstrap->assigned ? bootstrap->mux_channel : NRF_UPSTREAM_MUX_CHANNEL_NONE;
    status->target_address = bootstrap->ops.target_is_registered(bootstrap->ops.context)
        ? bootstrap->ops.target_address(bootstrap->ops.context)
        : 0U;
    status->lifecycle = lifecycle_to_wire(bootstrap->node_state->lifecycle);
    status->result = (uint8_t)bootstrap->result;
    if (bootstrap->assigned) {
        status->flags |= NRF_UPSTREAM_STATUS_FLAG_ASSIGNED;
    }
    if (bootstrap->ops.target_is_registered(bootstrap->ops.context)) {
        status->flags |= NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
    }
    return NRF_UPSTREAM_RESULT_OK;
}
