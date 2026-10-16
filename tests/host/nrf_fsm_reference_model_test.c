#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_bootstrap.h"
#include "nrf_cs_control_runtime.h"
#include "nrf_node_state.h"
#include "nrf_sensor_session.h"

typedef enum { M_ROLE_NONE = 31, M_ROLE_MAIN = 47, M_ROLE_REMOTE = 59 } model_role_t;
typedef enum { M_BOOTING = 11, M_READY = 23, M_UNAVAILABLE = 37 } model_lifecycle_t;
typedef enum { M_NODE_OK = 101, M_NODE_ARGUMENT = 103, M_NODE_NOT_READY = 107, M_NODE_CONFLICT = 109 } model_node_result_t;
typedef enum { M_NODE_READY, M_NODE_ASSIGN_MAIN, M_NODE_ASSIGN_REMOTE, M_NODE_UNAVAILABLE, M_NODE_AVAILABLE, M_NODE_RESET, M_NODE_ASSIGN_INVALID } model_node_event_t;

typedef struct {
    model_role_t role;
    model_lifecycle_t lifecycle;
} model_node_t;

static model_node_result_t model_node_apply(model_node_t *node, model_node_event_t event)
{
    model_role_t requested_role;
    switch (event) {
    case M_NODE_READY:
        node->lifecycle = M_READY;
        return M_NODE_OK;
    case M_NODE_ASSIGN_MAIN:
        requested_role = M_ROLE_MAIN;
        break;
    case M_NODE_ASSIGN_REMOTE:
        requested_role = M_ROLE_REMOTE;
        break;
    case M_NODE_ASSIGN_INVALID:
        return M_NODE_ARGUMENT;
    case M_NODE_UNAVAILABLE:
        if (node->lifecycle == M_BOOTING) return M_NODE_NOT_READY;
        node->lifecycle = M_UNAVAILABLE;
        return M_NODE_OK;
    case M_NODE_AVAILABLE:
        if (node->lifecycle == M_BOOTING) return M_NODE_NOT_READY;
        node->lifecycle = M_READY;
        return M_NODE_OK;
    case M_NODE_RESET:
        node->role = M_ROLE_NONE;
        node->lifecycle = M_BOOTING;
        return M_NODE_OK;
    default:
        return M_NODE_ARGUMENT;
    }
    if (node->lifecycle != M_READY) return M_NODE_NOT_READY;
    if (node->role != M_ROLE_NONE && node->role != requested_role) return M_NODE_CONFLICT;
    node->role = requested_role;
    return M_NODE_OK;
}

static model_role_t model_role_from_wire(uint8_t role)
{
    if (role == 1U) return M_ROLE_MAIN;
    if (role == 2U) return M_ROLE_REMOTE;
    return M_ROLE_NONE;
}

static int production_node_apply(nrf_node_state_t *node, model_node_event_t event)
{
    switch (event) {
    case M_NODE_READY: return nrf_node_state_mark_ready(node);
    case M_NODE_ASSIGN_MAIN: return nrf_node_state_assign_role(node, NRF_NODE_ROLE_MAIN);
    case M_NODE_ASSIGN_REMOTE: return nrf_node_state_assign_role(node, NRF_NODE_ROLE_REMOTE);
    case M_NODE_UNAVAILABLE: return nrf_node_state_mark_unavailable(node);
    case M_NODE_AVAILABLE: return nrf_node_state_mark_available(node);
    case M_NODE_RESET: nrf_node_state_reset(node); return NRF_NODE_STATE_OK;
    case M_NODE_ASSIGN_INVALID: return nrf_node_state_assign_role(node, (nrf_node_role_t)0x7FU);
    default: return NRF_NODE_STATE_ERR_ARGUMENT;
    }
}

static model_node_result_t model_node_result_from_production(int result)
{
    switch (result) {
    case NRF_NODE_STATE_OK: return M_NODE_OK;
    case NRF_NODE_STATE_ERR_ARGUMENT: return M_NODE_ARGUMENT;
    case NRF_NODE_STATE_ERR_NOT_READY: return M_NODE_NOT_READY;
    case NRF_NODE_STATE_ERR_ROLE_CONFLICT: return M_NODE_CONFLICT;
    default: return M_NODE_ARGUMENT;
    }
}

static void set_node_start(nrf_node_state_t *production, model_node_t *model,
                           model_role_t role, model_lifecycle_t lifecycle)
{
    nrf_node_state_init(production);
    model->role = M_ROLE_NONE;
    model->lifecycle = M_BOOTING;
    if (lifecycle != M_BOOTING) {
        assert(nrf_node_state_mark_ready(production) == NRF_NODE_STATE_OK);
        model->lifecycle = M_READY;
    }
    if (role != M_ROLE_NONE) {
        assert(nrf_node_state_assign_role(production,
            role == M_ROLE_MAIN ? NRF_NODE_ROLE_MAIN : NRF_NODE_ROLE_REMOTE) == NRF_NODE_STATE_OK);
        model->role = role;
    }
    if (lifecycle == M_UNAVAILABLE) {
        assert(nrf_node_state_mark_unavailable(production) == NRF_NODE_STATE_OK);
        model->lifecycle = M_UNAVAILABLE;
    }
}

/* characterization: independent reachable-state x event table for role and lifecycle transitions. */
static void test_node_transition_truth_table(void)
{
    static const struct { model_role_t role; model_lifecycle_t lifecycle; } starts[] = {
        {M_ROLE_NONE, M_BOOTING}, {M_ROLE_NONE, M_READY},
        {M_ROLE_MAIN, M_READY}, {M_ROLE_REMOTE, M_READY},
        {M_ROLE_NONE, M_UNAVAILABLE}, {M_ROLE_MAIN, M_UNAVAILABLE},
        {M_ROLE_REMOTE, M_UNAVAILABLE},
    };
    static const model_node_event_t events[] = {
        M_NODE_READY, M_NODE_ASSIGN_MAIN, M_NODE_ASSIGN_REMOTE,
        M_NODE_UNAVAILABLE, M_NODE_AVAILABLE, M_NODE_RESET, M_NODE_ASSIGN_INVALID,
    };
    for (size_t state = 0U; state < sizeof(starts) / sizeof(starts[0]); ++state) {
        for (size_t action = 0U; action < sizeof(events) / sizeof(events[0]); ++action) {
            nrf_node_state_t production;
            model_node_t model;
            set_node_start(&production, &model, starts[state].role, starts[state].lifecycle);
            const model_node_result_t expected = model_node_apply(&model, events[action]);
            const model_node_result_t actual = model_node_result_from_production(
                production_node_apply(&production, events[action]));
            assert(actual == expected);
            assert(model_role_from_wire((uint8_t)production.role) == model.role);
            assert((production.lifecycle == NRF_NODE_LIFECYCLE_BOOTING && model.lifecycle == M_BOOTING) ||
                   (production.lifecycle == NRF_NODE_LIFECYCLE_READY && model.lifecycle == M_READY) ||
                   (production.lifecycle == NRF_NODE_LIFECYCLE_UNAVAILABLE && model.lifecycle == M_UNAVAILABLE));
        }
    }
}

typedef enum {
    M_UP_OK = 201, M_UP_PENDING = 211, M_UP_INVALID_FRAME = 223,
    M_UP_BAD_CRC = 227, M_UP_REQUEST_INVALID = 229, M_UP_HW_MISMATCH = 233,
    M_UP_ROLE_INVALID = 239, M_UP_NODE_INVALID = 241, M_UP_CHANNEL_INVALID = 251,
    M_UP_ROLE_CONFLICT = 257, M_UP_REQUEST_CONFLICT = 263, M_UP_BUSY = 269,
} model_upstream_result_t;

typedef struct {
    uint32_t request_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[8];
    uint8_t role;
    uint8_t node_id;
    uint8_t mux_channel;
} model_assign_t;

typedef struct {
    model_node_t node;
    bool assigned;
    uint8_t node_id;
    uint8_t mux_channel;
    bool history_valid;
    uint32_t history_id;
    model_assign_t history_assign;
    model_upstream_result_t history_result;
    uint32_t status_id;
    model_upstream_result_t result;
    bool target_registered;
    uint8_t target_address;
} model_bootstrap_t;

typedef struct { bool registered; uint8_t address; unsigned readdresses; } mock_target_t;

static int mock_readdress(void *context, uint8_t address)
{
    mock_target_t *target = context;
    target->registered = true;
    target->address = address;
    ++target->readdresses;
    return 0;
}

static bool mock_target_registered(void *context) { return ((mock_target_t *)context)->registered; }
static uint8_t mock_target_address(void *context) { return ((mock_target_t *)context)->address; }
static void mock_reboot(void *context) { (void)context; }

static model_assign_t model_assign(uint32_t id, uint8_t role, uint8_t node_id, uint8_t channel)
{
    model_assign_t assign = {0};
    assign.request_id = id;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 1U; assign.hardware_id[1] = 2U;
    assign.hardware_id[2] = 3U; assign.hardware_id[3] = 4U;
    assign.role = role; assign.node_id = node_id; assign.mux_channel = channel;
    return assign;
}

static nrf_upstream_assign_t production_assign(const model_assign_t *model)
{
    nrf_upstream_assign_t assign = {0};
    assign.request_id = model->request_id;
    assign.hardware_id_length = model->hardware_id_length;
    memcpy(assign.hardware_id, model->hardware_id, sizeof(assign.hardware_id));
    assign.role = model->role == M_ROLE_MAIN ? NRF_UPSTREAM_ROLE_MAIN :
        model->role == M_ROLE_REMOTE ? NRF_UPSTREAM_ROLE_REMOTE : model->role;
    assign.node_id = model->node_id;
    assign.mux_channel = model->mux_channel;
    return assign;
}

static bool model_assign_equal(const model_assign_t *a, const model_assign_t *b)
{
    return a->request_id == b->request_id && a->hardware_id_length == b->hardware_id_length &&
        memcmp(a->hardware_id, b->hardware_id, sizeof(a->hardware_id)) == 0 &&
        a->role == b->role && a->node_id == b->node_id && a->mux_channel == b->mux_channel;
}

static model_upstream_result_t model_validate_assign(const model_assign_t *assign)
{
    if (assign->hardware_id_length == 0U || assign->hardware_id_length > sizeof(assign->hardware_id)) return M_UP_INVALID_FRAME;
    for (size_t index = assign->hardware_id_length; index < sizeof(assign->hardware_id); ++index) {
        if (assign->hardware_id[index] != 0U) return M_UP_INVALID_FRAME;
    }
    if (assign->request_id == 0U) return M_UP_REQUEST_INVALID;
    if (assign->role != M_ROLE_MAIN && assign->role != M_ROLE_REMOTE) return M_UP_ROLE_INVALID;
    if (assign->node_id == 0xFFU) return M_UP_NODE_INVALID;
    if ((assign->role == M_ROLE_MAIN && assign->mux_channel != 0xFFU) ||
        (assign->role == M_ROLE_REMOTE && assign->mux_channel > 7U)) return M_UP_CHANNEL_INVALID;
    return M_UP_OK;
}

static void model_bootstrap_result(model_bootstrap_t *model, uint32_t request_id,
                                   model_upstream_result_t result)
{
    model->status_id = request_id;
    model->result = result;
}

static model_upstream_result_t model_bootstrap_assign(model_bootstrap_t *model,
                                                       const model_assign_t *assign)
{
    model_upstream_result_t result = model_validate_assign(assign);
    if (result != M_UP_OK) { model_bootstrap_result(model, assign->request_id, result); return result; }
    if (assign->hardware_id_length != 4U || assign->hardware_id[0] != 1U ||
        assign->hardware_id[1] != 2U || assign->hardware_id[2] != 3U || assign->hardware_id[3] != 4U) {
        model_bootstrap_result(model, assign->request_id, M_UP_HW_MISMATCH); return M_UP_HW_MISMATCH;
    }
    if (model->history_valid && assign->request_id == model->history_id) {
        if (!model_assign_equal(assign, &model->history_assign)) {
            model_bootstrap_result(model, assign->request_id, M_UP_REQUEST_CONFLICT); return M_UP_REQUEST_CONFLICT;
        }
        if (model->history_result == M_UP_OK) {
            model_bootstrap_result(model, assign->request_id, M_UP_OK); return M_UP_OK;
        }
        if (model->history_result == M_UP_PENDING) {
            model_bootstrap_result(model, assign->request_id, M_UP_BUSY); return M_UP_BUSY;
        }
    }
    if (model->assigned) {
        if (assign->role == (uint8_t)model->node.role && assign->node_id == model->node_id &&
            assign->mux_channel == model->mux_channel) {
            model->history_valid = true; model->history_id = assign->request_id;
            model->history_assign = *assign; model->history_result = M_UP_OK;
            model_bootstrap_result(model, assign->request_id, M_UP_OK); return M_UP_OK;
        }
        model_bootstrap_result(model, assign->request_id, M_UP_ROLE_CONFLICT); return M_UP_ROLE_CONFLICT;
    }
    if (model->node.lifecycle != M_READY) { model_bootstrap_result(model, assign->request_id, M_UP_BUSY); return M_UP_BUSY; }
    if (model->node.role != M_ROLE_NONE) { model_bootstrap_result(model, assign->request_id, M_UP_ROLE_CONFLICT); return M_UP_ROLE_CONFLICT; }
    model->history_valid = true; model->history_id = assign->request_id;
    model->history_assign = *assign; model->history_result = M_UP_PENDING;
    model_bootstrap_result(model, assign->request_id, M_UP_PENDING);
    model->node.role = assign->role == M_ROLE_MAIN ? M_ROLE_MAIN : M_ROLE_REMOTE;
    model->assigned = true; model->node_id = assign->node_id; model->mux_channel = assign->mux_channel;
    model->target_registered = true;
    model->target_address = assign->role == M_ROLE_MAIN ? 0x31U : 0x32U;
    model->history_result = M_UP_OK;
    model_bootstrap_result(model, assign->request_id, M_UP_OK);
    return M_UP_OK;
}

static model_upstream_result_t model_upstream_result_from_production(int result)
{
    switch (result) {
    case NRF_UPSTREAM_RESULT_OK: return M_UP_OK;
    case NRF_UPSTREAM_RESULT_PENDING: return M_UP_PENDING;
    case NRF_UPSTREAM_RESULT_INVALID_FRAME: return M_UP_INVALID_FRAME;
    case NRF_UPSTREAM_RESULT_BAD_CRC: return M_UP_BAD_CRC;
    case NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID: return M_UP_REQUEST_INVALID;
    case NRF_UPSTREAM_RESULT_HW_ID_MISMATCH: return M_UP_HW_MISMATCH;
    case NRF_UPSTREAM_RESULT_ROLE_INVALID: return M_UP_ROLE_INVALID;
    case NRF_UPSTREAM_RESULT_NODE_ID_INVALID: return M_UP_NODE_INVALID;
    case NRF_UPSTREAM_RESULT_CHANNEL_INVALID: return M_UP_CHANNEL_INVALID;
    case NRF_UPSTREAM_RESULT_ROLE_CONFLICT: return M_UP_ROLE_CONFLICT;
    case NRF_UPSTREAM_RESULT_REQUEST_CONFLICT: return M_UP_REQUEST_CONFLICT;
    case NRF_UPSTREAM_RESULT_BUSY: return M_UP_BUSY;
    default: return M_UP_INVALID_FRAME;
    }
}

static void compare_bootstrap(const model_bootstrap_t *model, const nrf_node_state_t *node,
                              const nrf_bootstrap_t *bootstrap, const mock_target_t *target)
{
    nrf_upstream_status_t status;
    assert(nrf_bootstrap_make_status(bootstrap, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(model_role_from_wire((uint8_t)node->role) == model->node.role);
    assert((node->lifecycle == NRF_NODE_LIFECYCLE_READY) == (model->node.lifecycle == M_READY));
    assert(bootstrap->assigned == model->assigned);
    assert(bootstrap->node_id == (model->assigned ? model->node_id : 0xFFU));
    assert(bootstrap->mux_channel == (model->assigned ? model->mux_channel : 0xFFU));
    assert(bootstrap->status_request_id == model->status_id);
    assert(model_upstream_result_from_production(bootstrap->result) == model->result);
    assert(status.request_id == model->status_id);
    assert(model_upstream_result_from_production(status.result) == model->result);
    assert(status.target_address == target->address && target->registered == model->target_registered);
}

/* characterization: independent ASSIGN replay/conflict model compares state after every API call. */
static void test_bootstrap_assign_model(void)
{
    static const uint8_t hardware_id[4] = {1U, 2U, 3U, 4U};
    nrf_node_state_t node;
    nrf_bootstrap_t bootstrap;
    mock_target_t target = {.registered = true, .address = 0x30U};
    const nrf_bootstrap_ops_t ops = {
        .readdress = mock_readdress, .target_is_registered = mock_target_registered,
        .target_address = mock_target_address, .cold_reboot = mock_reboot, .context = &target,
    };
    model_bootstrap_t model = {0};
    model.node.role = M_ROLE_NONE; model.node.lifecycle = M_READY;
    model.target_registered = true; model.target_address = 0x30U;
    model.result = M_UP_OK;
    nrf_node_state_init(&node);
    assert(nrf_node_state_mark_ready(&node) == NRF_NODE_STATE_OK);
    assert(nrf_bootstrap_init(&bootstrap, &node, hardware_id, sizeof(hardware_id), &ops) == NRF_UPSTREAM_RESULT_OK);
    const model_assign_t events[] = {
        model_assign(1U, M_ROLE_REMOTE, 3U, 2U),
        model_assign(1U, M_ROLE_REMOTE, 3U, 2U),
        model_assign(1U, M_ROLE_REMOTE, 3U, 1U),
        model_assign(2U, M_ROLE_MAIN, 4U, 0xFFU),
        model_assign(3U, M_ROLE_REMOTE, 4U, 2U),
        model_assign(4U, 99U, 4U, 2U),
        model_assign(5U, M_ROLE_REMOTE, 0xFFU, 2U),
        model_assign(6U, M_ROLE_REMOTE, 4U, 8U),
        model_assign(0U, M_ROLE_REMOTE, 4U, 2U),
    };
    for (size_t index = 0U; index < sizeof(events) / sizeof(events[0]); ++index) {
        const model_upstream_result_t expected = model_bootstrap_assign(&model, &events[index]);
        const nrf_upstream_assign_t request = production_assign(&events[index]);
        const model_upstream_result_t actual = model_upstream_result_from_production(
            nrf_bootstrap_process_assign(&bootstrap, &request));
        assert(actual == expected);
        compare_bootstrap(&model, &node, &bootstrap, &target);
    }
}

typedef struct {
    uint32_t request_id;
    uint32_t sequence;
    uint8_t result;
    uint8_t flags;
    uint8_t valid_mask;
    uint8_t fault_mask;
    int16_t accel_z;
} model_snapshot_t;

typedef struct {
    bool active;
    uint8_t node_id;
    uint8_t channel;
    bool pending;
    uint32_t active_id;
    bool completed_valid;
    uint32_t completed_id;
    model_snapshot_t published;
} model_sensor_t;

typedef enum { M_ACT_NEW = 311, M_ACT_IDEMPOTENT = 313, M_ACT_CONFLICT = 317, M_ACT_INVALID = 331 } model_sensor_activate_result_t;
typedef enum { M_REQ_ACCEPTED = 337, M_REQ_IDEMPOTENT = 347, M_REQ_BUSY = 349, M_REQ_NOT_REMOTE = 353, M_REQ_INVALID = 359 } model_sensor_request_result_t;

static void model_sensor_init(model_sensor_t *model)
{
    memset(model, 0, sizeof(*model));
    model->published.result = 3U;
}

static model_sensor_activate_result_t model_sensor_activate(model_sensor_t *model, const model_assign_t *assign)
{
    if (model_validate_assign(assign) != M_UP_OK || assign->role != M_ROLE_REMOTE) return M_ACT_INVALID;
    if (model->active) return model->node_id == assign->node_id && model->channel == assign->mux_channel ? M_ACT_IDEMPOTENT : M_ACT_CONFLICT;
    model->active = true; model->node_id = assign->node_id; model->channel = assign->mux_channel;
    model->pending = true; model->active_id = 0U; model->completed_valid = false; model->completed_id = 0U;
    memset(&model->published, 0, sizeof(model->published)); model->published.result = 1U;
    return M_ACT_NEW;
}

static model_sensor_request_result_t model_sensor_begin(model_sensor_t *model, uint32_t request_id)
{
    if (request_id == 0U) return M_REQ_INVALID;
    if (!model->active) return M_REQ_NOT_REMOTE;
    if (model->pending) return model->active_id == request_id ? M_REQ_IDEMPOTENT : M_REQ_BUSY;
    if (model->completed_valid && model->completed_id == request_id) return M_REQ_IDEMPOTENT;
    const uint8_t flags = model->published.flags & 0x07U;
    const uint32_t sequence = model->published.sequence;
    memset(&model->published, 0, sizeof(model->published));
    model->published.request_id = request_id; model->published.sequence = sequence;
    model->published.result = 1U; model->published.flags = flags;
    model->pending = true; model->active_id = request_id;
    return M_REQ_ACCEPTED;
}

static bool model_sensor_complete(model_sensor_t *model, uint32_t request_id, const model_snapshot_t *snapshot)
{
    if (!model->active || !model->pending || model->active_id != request_id ||
        snapshot->request_id != request_id || snapshot->result == 1U) return false;
    model->published = *snapshot; model->pending = false; model->active_id = 0U;
    if (request_id != 0U) { model->completed_valid = true; model->completed_id = request_id; }
    return true;
}

static bool model_sensor_busy(model_sensor_t *model, uint32_t request_id)
{
    if (request_id == 0U || !model->pending || model->active_id != request_id) return false;
    model->published.result = 6U; model->pending = false; model->active_id = 0U;
    model->completed_valid = true; model->completed_id = request_id;
    return true;
}

static nrf_upstream_sensor_snapshot_t production_snapshot(const model_snapshot_t *model)
{
    nrf_upstream_sensor_snapshot_t snapshot = {0};
    snapshot.request_id = model->request_id; snapshot.sequence = model->sequence;
    snapshot.result = (nrf_upstream_sensor_result_t)model->result; snapshot.flags = model->flags;
    snapshot.valid_mask = model->valid_mask; snapshot.fault_mask = model->fault_mask;
    snapshot.accel_mg[2] = model->accel_z;
    return snapshot;
}

static void compare_sensor(const model_sensor_t *model, const nrf_sensor_session_t *production)
{
    nrf_upstream_sensor_snapshot_t published;
    nrf_sensor_session_get_published(production, &published);
    assert(production->remote_active == model->active && production->node_id == model->node_id);
    assert(production->mux_channel == model->channel && production->request_pending == model->pending);
    assert(production->active_request_id == model->active_id);
    assert(production->completed_request_valid == model->completed_valid);
    assert(production->completed_request_id == model->completed_id);
    assert(published.request_id == model->published.request_id && published.sequence == model->published.sequence);
    assert((uint8_t)published.result == model->published.result && published.flags == model->published.flags);
    assert(published.valid_mask == model->published.valid_mask && published.fault_mask == model->published.fault_mask);
    assert(published.accel_mg[2] == model->published.accel_z);
}

/* characterization: independent sensor activation/request/completion model compares after each event. */
static void test_sensor_session_model(void)
{
    model_sensor_t model;
    nrf_sensor_session_t production;
    model_sensor_init(&model); nrf_sensor_session_init(&production);
    assert(nrf_sensor_session_begin_request(&production, 10U) == NRF_SENSOR_SESSION_REQUEST_NOT_REMOTE);
    assert(model_sensor_begin(&model, 10U) == M_REQ_NOT_REMOTE); compare_sensor(&model, &production);

    const model_assign_t remote = model_assign(1U, M_ROLE_REMOTE, 3U, 2U);
    const model_assign_t moved = model_assign(2U, M_ROLE_REMOTE, 3U, 3U);
    const model_assign_t main = model_assign(3U, M_ROLE_MAIN, 3U, 0xFFU);
    assert(nrf_sensor_session_activate_remote(&production, &(nrf_upstream_assign_t){0}) == NRF_SENSOR_SESSION_ACTIVATE_INVALID);
    assert(model_sensor_activate(&model, &(model_assign_t){0}) == M_ACT_INVALID); compare_sensor(&model, &production);
    assert(nrf_sensor_session_activate_remote(&production, &(nrf_upstream_assign_t){
        .request_id=remote.request_id, .hardware_id_length=remote.hardware_id_length,
        .hardware_id={1U,2U,3U,4U}, .role=NRF_UPSTREAM_ROLE_REMOTE,
        .node_id=remote.node_id, .mux_channel=remote.mux_channel}) == NRF_SENSOR_SESSION_ACTIVATE_NEW);
    assert(model_sensor_activate(&model, &remote) == M_ACT_NEW); compare_sensor(&model, &production);
    const nrf_upstream_assign_t same = production_assign(&remote);
    const nrf_upstream_assign_t moved_production = production_assign(&moved);
    const nrf_upstream_assign_t main_production = production_assign(&main);
    assert(nrf_sensor_session_activate_remote(&production, &same) == NRF_SENSOR_SESSION_ACTIVATE_IDEMPOTENT);
    assert(model_sensor_activate(&model, &remote) == M_ACT_IDEMPOTENT); compare_sensor(&model, &production);
    assert(nrf_sensor_session_activate_remote(&production, &moved_production) == NRF_SENSOR_SESSION_ACTIVATE_CONFLICT);
    assert(model_sensor_activate(&model, &moved) == M_ACT_CONFLICT); compare_sensor(&model, &production);
    assert(nrf_sensor_session_activate_remote(&production, &main_production) == NRF_SENSOR_SESSION_ACTIVATE_INVALID);
    assert(model_sensor_activate(&model, &main) == M_ACT_INVALID); compare_sensor(&model, &production);

    assert((int)nrf_sensor_session_begin_request(&production, 0U) == -1);
    assert(model_sensor_begin(&model, 0U) == M_REQ_INVALID); compare_sensor(&model, &production);
    model_snapshot_t initial = {.request_id=0U, .sequence=1U, .result=2U, .flags=7U, .valid_mask=1U, .accel_z=1000};
    const nrf_upstream_sensor_snapshot_t initial_production = production_snapshot(&initial);
    assert(nrf_sensor_session_complete(&production, 0U, &initial_production));
    assert(model_sensor_complete(&model, 0U, &initial)); compare_sensor(&model, &production);
    assert(nrf_sensor_session_begin_request(&production, 55U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
    assert(model_sensor_begin(&model, 55U) == M_REQ_ACCEPTED); compare_sensor(&model, &production);
    assert(nrf_sensor_session_begin_request(&production, 55U) == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT);
    assert(model_sensor_begin(&model, 55U) == M_REQ_IDEMPOTENT); compare_sensor(&model, &production);
    assert(nrf_sensor_session_begin_request(&production, 56U) == NRF_SENSOR_SESSION_REQUEST_BUSY);
    assert(model_sensor_begin(&model, 56U) == M_REQ_BUSY); compare_sensor(&model, &production);
    model_snapshot_t final = {.request_id=55U, .sequence=2U, .result=2U, .flags=9U,
                              .valid_mask=1U, .fault_mask=2U, .accel_z=999};
    nrf_upstream_sensor_snapshot_t final_production = production_snapshot(&final);
    assert(!nrf_sensor_session_complete(&production, 54U, &final_production));
    assert(!model_sensor_complete(&model, 54U, &final)); compare_sensor(&model, &production);
    final_production.request_id = 56U;
    assert(!nrf_sensor_session_complete(&production, 55U, &final_production));
    final_production.request_id = 55U;
    assert(nrf_sensor_session_complete(&production, 55U, &final_production));
    assert(model_sensor_complete(&model, 55U, &final)); compare_sensor(&model, &production);
    assert(!nrf_sensor_session_complete(&production, 55U, &final_production));
    assert(!model_sensor_complete(&model, 55U, &final)); compare_sensor(&model, &production);
    assert(nrf_sensor_session_begin_request(&production, 55U) == NRF_SENSOR_SESSION_REQUEST_IDEMPOTENT);
    assert(model_sensor_begin(&model, 55U) == M_REQ_IDEMPOTENT); compare_sensor(&model, &production);
    assert(nrf_sensor_session_begin_request(&production, 57U) == NRF_SENSOR_SESSION_REQUEST_ACCEPTED);
    assert(model_sensor_begin(&model, 57U) == M_REQ_ACCEPTED); compare_sensor(&model, &production);
    assert(!nrf_sensor_session_mark_enqueue_busy(&production, 58U));
    assert(!model_sensor_busy(&model, 58U)); compare_sensor(&model, &production);
    assert(nrf_sensor_session_mark_enqueue_busy(&production, 57U));
    assert(model_sensor_busy(&model, 57U)); compare_sensor(&model, &production);
}

typedef enum { M_CS_IDLE = 401, M_CS_RUNNING = 409, M_CS_COMPLETE = 419, M_CS_FAILED = 421 } model_cs_state_t;
typedef enum { M_CS_OK = 431, M_CS_PENDING = 433, M_CS_BUSY = 439, M_CS_STALE = 443, M_CS_INVALID = 449, M_CS_INTERNAL = 457 } model_cs_command_t;
typedef enum { M_OUTCOME_SUCCESS = 461, M_OUTCOME_TIMEOUT = 463, M_OUTCOME_TRANSPORT = 467, M_OUTCOME_OUTLIER = 479 } model_outcome_t;

typedef struct {
    uint32_t cycle, generation, attempt;
    uint8_t pair, attempt_number, initiator, reflector;
    uint32_t timeout;
} model_step_t;

typedef struct { uint32_t id; model_step_t step; } model_request_t;
typedef struct { model_request_t request; model_cs_state_t state; model_cs_command_t command; model_outcome_t outcome; int32_t distance; uint16_t confidence, flags; } model_cs_result_t;
typedef struct {
    uint8_t local_node;
    bool assigned, active;
    model_request_t active_request;
    bool terminal_valid;
    model_cs_result_t terminal;
    bool immediate_valid;
    model_cs_result_t immediate;
    bool enqueue_valid;
    model_cs_result_t enqueue;
    bool selector_armed, selector_stale;
    model_request_t selector;
} model_cs_runtime_t;

typedef enum { M_EV_ASSIGN, M_EV_ACCEPT, M_EV_COMPLETE, M_EV_RECORD_ENQUEUE, M_EV_SELECT, M_EV_TAKE } model_cs_event_kind_t;
typedef struct { model_cs_event_kind_t kind; uint8_t node; model_request_t request; model_step_t step; model_outcome_t outcome; model_cs_command_t command; int32_t distance; uint16_t confidence, flags; } model_cs_event_t;

static bool model_step_equal(const model_step_t *a, const model_step_t *b)
{
    return a->cycle == b->cycle && a->generation == b->generation && a->attempt == b->attempt &&
        a->pair == b->pair && a->attempt_number == b->attempt_number && a->initiator == b->initiator &&
        a->reflector == b->reflector && a->timeout == b->timeout;
}

static bool model_request_equal(const model_request_t *a, const model_request_t *b)
{
    return a->id == b->id && model_step_equal(&a->step, &b->step);
}

static bool model_request_valid(const model_request_t *request)
{
    return request->id != 0U && request->step.cycle != 0U && request->step.generation != 0U &&
        request->step.attempt != 0U && request->step.pair < 10U && request->step.attempt_number != 0U &&
        request->step.initiator != 0xFFU && request->step.reflector != 0xFFU &&
        request->step.initiator != request->step.reflector && request->step.timeout != 0U;
}

static nrf_cs_control_request_t production_request(const model_request_t *model)
{
    nrf_cs_control_request_t request = {0};
    request.request_id = model->id;
    request.step.cycle_id = model->step.cycle; request.step.membership_generation = model->step.generation;
    request.step.attempt_id = model->step.attempt; request.step.pair_index = model->step.pair;
    request.step.attempt_number = model->step.attempt_number; request.step.initiator = model->step.initiator;
    request.step.reflector = model->step.reflector; request.step.timeout_ms = model->step.timeout;
    return request;
}

static model_request_t model_request_from_production(const nrf_cs_control_request_t *request)
{
    model_request_t model = {0};
    model.id = request->request_id; model.step.cycle = request->step.cycle_id;
    model.step.generation = request->step.membership_generation; model.step.attempt = request->step.attempt_id;
    model.step.pair = request->step.pair_index; model.step.attempt_number = request->step.attempt_number;
    model.step.initiator = request->step.initiator; model.step.reflector = request->step.reflector;
    model.step.timeout = request->step.timeout_ms;
    return model;
}

static model_cs_result_t model_cs_running(const model_request_t *request)
{
    return (model_cs_result_t){.request=*request, .state=M_CS_RUNNING, .command=M_CS_PENDING, .outcome=M_OUTCOME_TRANSPORT};
}

static model_cs_result_t model_cs_rejected(const model_request_t *request, model_cs_command_t command)
{
    return (model_cs_result_t){.request=*request, .state=M_CS_FAILED, .command=command, .outcome=M_OUTCOME_TRANSPORT};
}

static bool model_result_matches(const model_request_t *request, const model_cs_result_t *result)
{
    return model_request_equal(request, &result->request);
}

static int model_cs_assign(model_cs_runtime_t *model, uint8_t node)
{
    if (node == 0xFFU) return -1;
    if (model->active && model->local_node != node) return -2;
    model->local_node = node; model->assigned = true; return 0;
}

static int model_cs_accept(model_cs_runtime_t *model, const model_request_t *request,
                           model_cs_result_t *out, bool *start_required)
{
    if (!model_request_valid(request)) return -1;
    *start_required = false;
    if (!model->assigned || request->step.initiator != model->local_node) {
        *out = model_cs_rejected(request, M_CS_INVALID); model->immediate = *out; model->immediate_valid = true; return 0;
    }
    if (model->active) {
        if (model_request_equal(request, &model->active_request)) *out = model_cs_running(request);
        else { *out = model_cs_rejected(request, M_CS_BUSY); model->immediate = *out; model->immediate_valid = true; }
        return 0;
    }
    if (model->terminal_valid && model_result_matches(request, &model->terminal)) { *out = model->terminal; return 0; }
    model->immediate_valid = false; model->active = true; model->active_request = *request;
    *out = model_cs_running(request); *start_required = true; return 0;
}

static int model_cs_record_enqueue(model_cs_runtime_t *model, const model_request_t *request, model_cs_command_t command)
{
    if (!model_request_valid(request) || (command != M_CS_BUSY && command != M_CS_INTERNAL)) return -1;
    model->enqueue = model_cs_rejected(request, command); model->enqueue_valid = true; return 0;
}

static int model_cs_complete(model_cs_runtime_t *model, const model_step_t *step,
                             model_outcome_t outcome, int32_t distance, uint16_t confidence, uint16_t flags)
{
    if (!model->active || !model_step_equal(step, &model->active_request.step)) return -2;
    model->terminal.request = model->active_request;
    model->terminal.state = outcome == M_OUTCOME_SUCCESS ? M_CS_COMPLETE : M_CS_FAILED;
    model->terminal.command = M_CS_OK; model->terminal.outcome = outcome;
    model->terminal.distance = distance; model->terminal.confidence = confidence; model->terminal.flags = flags;
    model->terminal_valid = true; model->active = false; memset(&model->active_request, 0, sizeof(model->active_request));
    return 0;
}

static int model_cs_select(model_cs_runtime_t *model, const model_request_t *request)
{
    if (!model_request_valid(request)) return -1;
    model->selector = *request; model->selector_armed = true;
    if ((model->active && model_request_equal(request, &model->active_request)) ||
        (model->terminal_valid && model_result_matches(request, &model->terminal)) ||
        (model->immediate_valid && model_result_matches(request, &model->immediate)) ||
        (model->enqueue_valid && model_result_matches(request, &model->enqueue))) {
        model->selector_stale = false; return 0;
    }
    model->selector_stale = true; return -2;
}

static bool model_cs_take(model_cs_runtime_t *model, model_cs_result_t *out)
{
    if (!model->selector_armed) return false;
    model->selector_armed = false;
    if (model->selector_stale) { *out = model_cs_rejected(&model->selector, M_CS_STALE); model->selector_stale = false; return true; }
    if (model->active && model_request_equal(&model->selector, &model->active_request)) { *out = model_cs_running(&model->active_request); return true; }
    if (model->terminal_valid && model_result_matches(&model->selector, &model->terminal)) { *out = model->terminal; return true; }
    if (model->immediate_valid && model_result_matches(&model->selector, &model->immediate)) { *out = model->immediate; model->immediate_valid = false; return true; }
    if (model->enqueue_valid && model_result_matches(&model->selector, &model->enqueue)) { *out = model->enqueue; model->enqueue_valid = false; return true; }
    return false;
}

static model_cs_state_t model_state_from_production(uint8_t value)
{
    switch (value) {
    case NRF_CS_CONTROL_STATE_IDLE: return M_CS_IDLE;
    case NRF_CS_CONTROL_STATE_RUNNING: return M_CS_RUNNING;
    case NRF_CS_CONTROL_STATE_COMPLETE: return M_CS_COMPLETE;
    default: return M_CS_FAILED;
    }
}

static model_cs_command_t model_command_from_production(uint8_t value)
{
    switch (value) {
    case NRF_CS_CONTROL_RESULT_OK: return M_CS_OK;
    case NRF_CS_CONTROL_RESULT_PENDING: return M_CS_PENDING;
    case NRF_CS_CONTROL_RESULT_BUSY: return M_CS_BUSY;
    case NRF_CS_CONTROL_RESULT_STALE: return M_CS_STALE;
    case NRF_CS_CONTROL_RESULT_INVALID: return M_CS_INVALID;
    default: return M_CS_INTERNAL;
    }
}

static model_outcome_t model_outcome_from_production(uint8_t value)
{
    switch (value) {
    case NRF_CS_OUTCOME_SUCCESS: return M_OUTCOME_SUCCESS;
    case NRF_CS_OUTCOME_TIMEOUT: return M_OUTCOME_TIMEOUT;
    case NRF_CS_OUTCOME_TRANSPORT_ERROR: return M_OUTCOME_TRANSPORT;
    default: return M_OUTCOME_OUTLIER;
    }
}

static model_cs_result_t model_cs_result_from_production(const nrf_cs_control_result_t *result)
{
    model_cs_result_t model = {0};
    model.request = model_request_from_production(&(nrf_cs_control_request_t){.request_id=result->request_id,.step=result->step});
    model.state = model_state_from_production((uint8_t)result->state);
    model.command = model_command_from_production((uint8_t)result->command_result);
    model.outcome = model_outcome_from_production((uint8_t)result->measurement.outcome);
    model.distance = result->measurement.distance_mm;
    model.confidence = result->measurement.confidence; model.flags = result->measurement.provider_quality_flags;
    return model;
}

static uint8_t production_state(model_cs_state_t value)
{
    switch (value) {
    case M_CS_IDLE: return NRF_CS_CONTROL_STATE_IDLE;
    case M_CS_RUNNING: return NRF_CS_CONTROL_STATE_RUNNING;
    case M_CS_COMPLETE: return NRF_CS_CONTROL_STATE_COMPLETE;
    default: return NRF_CS_CONTROL_STATE_FAILED;
    }
}

static uint8_t production_command(model_cs_command_t value)
{
    switch (value) {
    case M_CS_OK: return NRF_CS_CONTROL_RESULT_OK;
    case M_CS_PENDING: return NRF_CS_CONTROL_RESULT_PENDING;
    case M_CS_BUSY: return NRF_CS_CONTROL_RESULT_BUSY;
    case M_CS_STALE: return NRF_CS_CONTROL_RESULT_STALE;
    case M_CS_INVALID: return NRF_CS_CONTROL_RESULT_INVALID;
    default: return NRF_CS_CONTROL_RESULT_INTERNAL_ERROR;
    }
}

static uint8_t production_outcome(model_outcome_t value)
{
    switch (value) {
    case M_OUTCOME_SUCCESS: return NRF_CS_OUTCOME_SUCCESS;
    case M_OUTCOME_TIMEOUT: return NRF_CS_OUTCOME_TIMEOUT;
    case M_OUTCOME_TRANSPORT: return NRF_CS_OUTCOME_TRANSPORT_ERROR;
    default: return NRF_CS_OUTCOME_OUTLIER;
    }
}

static void compare_cs_result(const model_cs_result_t *model, const nrf_cs_control_result_t *production)
{
    const model_cs_result_t actual = model_cs_result_from_production(production);
    assert(model_request_equal(&model->request, &actual.request));
    assert(model->state == actual.state && model->command == actual.command && model->outcome == actual.outcome);
    assert(model->distance == actual.distance && model->confidence == actual.confidence && model->flags == actual.flags);
}

static void compare_cs_state(const model_cs_runtime_t *model, const nrf_cs_control_runtime_t *production)
{
    assert(model->local_node == production->local_node_id && model->assigned == production->assigned);
    assert(model->active == production->active && model->terminal_valid == production->terminal_valid);
    assert(model->immediate_valid == production->immediate_valid && model->enqueue_valid == production->enqueue_result_valid);
    assert(model->selector_armed == production->selector_armed && model->selector_stale == production->selector_stale);
    if (model->active) {
        const nrf_cs_control_request_t request = {
            .request_id = production->active_request.request_id,
            .step = production->active_request.step,
        };
        const model_request_t active_request = model_request_from_production(&request);
        assert(model_request_equal(&model->active_request, &active_request));
    }
    if (model->terminal_valid) compare_cs_result(&model->terminal, &production->terminal_result);
    if (model->immediate_valid) compare_cs_result(&model->immediate, &production->immediate_result);
    if (model->enqueue_valid) compare_cs_result(&model->enqueue, &production->enqueue_result);
}

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U; value ^= value >> 17U; value ^= value << 5U;
    *state = value == 0U ? 0x9E3779B9U : value;
    return *state;
}

#define MODEL_MAX_TRACE 64U
static model_cs_event_t s_trace[MODEL_MAX_TRACE];

static model_request_t make_model_request(uint32_t id, uint32_t attempt, uint32_t generation)
{
    model_request_t request = {0};
    request.id = id; request.step.cycle = 1U; request.step.generation = generation;
    request.step.attempt = attempt; request.step.pair = 0U; request.step.attempt_number = 1U;
    request.step.initiator = 2U; request.step.reflector = 3U; request.step.timeout = 500U;
    return request;
}

static model_cs_event_t make_seeded_event(uint32_t *random, size_t index)
{
    model_cs_event_t event = {0};
    const uint32_t value = random_next(random);
    event.kind = (model_cs_event_kind_t)(value % 6U);
    event.node = (uint8_t)((value >> 8U) % 5U);
    event.request = make_model_request(((value >> 16U) % 5U) + 1U,
        ((value >> 4U) % 7U) + 1U, ((value >> 12U) % 3U) + 1U);
    if ((value & 0x40000000U) != 0U) event.request.step.pair = 10U;
    if ((value & 0x20000000U) != 0U) event.request.step.initiator = 0xFFU;
    if ((value & 0x10000000U) != 0U) event.request.id = 0U;
    event.step = event.request.step;
    if ((value & 0x08000000U) != 0U) event.step.generation++;
    switch ((value >> 20U) % 4U) {
    case 0U: event.outcome = M_OUTCOME_SUCCESS; break;
    case 1U: event.outcome = M_OUTCOME_TIMEOUT; break;
    case 2U: event.outcome = M_OUTCOME_TRANSPORT; break;
    default: event.outcome = M_OUTCOME_OUTLIER; break;
    }
    event.command = ((value >> 24U) & 1U) == 0U ? M_CS_BUSY : M_CS_INTERNAL;
    event.distance = (int32_t)(value & 0x7FFFU); event.confidence = (uint16_t)(value % 1001U);
    event.flags = (uint16_t)(value >> 16U);
    if (index < 10U) event.node = 2U;
    return event;
}

static bool same_model_result(const model_cs_result_t *a, const model_cs_result_t *b)
{
    return model_request_equal(&a->request, &b->request) && a->state == b->state &&
        a->command == b->command && a->outcome == b->outcome && a->distance == b->distance &&
        a->confidence == b->confidence && a->flags == b->flags;
}

static bool run_cs_trace(const model_cs_event_t *trace, size_t count, size_t *failed_at)
{
    model_cs_runtime_t model = {.local_node=0xFFU};
    nrf_cs_control_runtime_t production;
    assert(nrf_cs_control_runtime_init(&production) == 0);
    for (size_t index = 0U; index < count; ++index) {
        const model_cs_event_t *event = &trace[index];
        bool failed = false;
        switch (event->kind) {
        case M_EV_ASSIGN: {
            const int expected = model_cs_assign(&model, event->node);
            const int actual = nrf_cs_control_runtime_assign_node(&production, event->node);
            failed = actual != expected;
            break;
        }
        case M_EV_ACCEPT: {
            model_cs_result_t expected_status = {0};
            nrf_cs_control_result_t actual_status = {0};
            bool expected_start = false, actual_start = false;
            const nrf_cs_control_request_t request = production_request(&event->request);
            const int expected = model_cs_accept(&model, &event->request, &expected_status, &expected_start);
            const int actual = nrf_cs_control_runtime_accept_execute(&production, &request, &actual_status, &actual_start);
            failed = actual != expected || actual_start != expected_start;
            if (!failed && actual == 0) { model_cs_result_t mapped = model_cs_result_from_production(&actual_status); failed = !same_model_result(&expected_status, &mapped); }
            break;
        }
        case M_EV_RECORD_ENQUEUE: {
            const model_cs_command_t command = event->command;
            const int expected = model_cs_record_enqueue(&model, &event->request, command);
            const nrf_cs_control_request_t request = production_request(&event->request);
            const int actual = nrf_cs_control_runtime_record_enqueue_result(&production,
                &request, production_command(command));
            failed = actual != expected;
            break;
        }
        case M_EV_COMPLETE: {
            nrf_cs_measurement_t measurement = {0};
            measurement.outcome = production_outcome(event->outcome); measurement.distance_mm = event->distance;
            measurement.confidence = event->confidence; measurement.provider_quality_flags = event->flags;
            const model_request_t completion_base = model.active ? model.active_request : event->request;
            const nrf_cs_step_t step = production_request(&completion_base).step;
            model_step_t completion_step = event->step;
            if ((random_next(&(uint32_t){(uint32_t)index + 1U}) & 1U) != 0U && model.active) completion_step = model.active_request.step;
            nrf_cs_step_t actual_step = step;
            actual_step.cycle_id = completion_step.cycle; actual_step.membership_generation = completion_step.generation;
            actual_step.attempt_id = completion_step.attempt; actual_step.pair_index = completion_step.pair;
            actual_step.attempt_number = completion_step.attempt_number; actual_step.initiator = completion_step.initiator;
            actual_step.reflector = completion_step.reflector; actual_step.timeout_ms = completion_step.timeout;
            const int expected = model_cs_complete(&model, &completion_step, event->outcome,
                event->distance, event->confidence, event->flags);
            const int actual = nrf_cs_control_runtime_complete(&production, &actual_step, &measurement);
            failed = actual != expected;
            break;
        }
        case M_EV_SELECT: {
            const int expected = model_cs_select(&model, &event->request);
            const nrf_cs_control_request_t request = production_request(&event->request);
            const int actual = nrf_cs_control_runtime_select_result(&production, &request);
            failed = actual != expected;
            break;
        }
        case M_EV_TAKE: {
            model_cs_result_t expected = {0}; nrf_cs_control_result_t actual = {0};
            const bool expected_taken = model_cs_take(&model, &expected);
            const bool actual_taken = nrf_cs_control_runtime_take_selected(&production, &actual);
            failed = actual_taken != expected_taken;
            if (!failed && expected_taken) { model_cs_result_t mapped = model_cs_result_from_production(&actual); failed = !same_model_result(&expected, &mapped); }
            break;
        }
        default: failed = true; break;
        }
        if (!failed) {
            if (model.active != production.active || model.assigned != production.assigned ||
                model.local_node != production.local_node_id || model.terminal_valid != production.terminal_valid ||
                model.immediate_valid != production.immediate_valid || model.enqueue_valid != production.enqueue_result_valid ||
                model.selector_armed != production.selector_armed || model.selector_stale != production.selector_stale) failed = true;
        }
        if (failed) { *failed_at = index; return false; }
        compare_cs_state(&model, &production);
    }
    return true;
}

static void print_model_step(FILE *stream, const model_step_t *step)
{
    (void)fprintf(stream, "%u/%u/%u/%u/%u/%u/%u/%u",
        (unsigned)step->cycle, (unsigned)step->generation, (unsigned)step->attempt,
        (unsigned)step->pair, (unsigned)step->attempt_number, (unsigned)step->initiator,
        (unsigned)step->reflector, (unsigned)step->timeout);
}

static void print_trace(uint32_t seed, const model_cs_event_t *trace, size_t count, const char *label)
{
    (void)fprintf(stderr, "%s seed=0x%08X count=%zu tape=", label, (unsigned)seed, count);
    for (size_t index = 0U; index < count; ++index) {
        (void)fprintf(stderr, "%s%zu:{kind=%u,node=%u,id=%u,req_step=", index == 0U ? "" : ",",
            index, (unsigned)trace[index].kind, (unsigned)trace[index].node,
            (unsigned)trace[index].request.id);
        print_model_step(stderr, &trace[index].request.step);
        (void)fprintf(stderr, ",event_step=");
        print_model_step(stderr, &trace[index].step);
        (void)fprintf(stderr, ",outcome=%u,command=%u,distance=%ld,confidence=%u,flags=%u}",
            (unsigned)trace[index].outcome, (unsigned)trace[index].command,
            (long)trace[index].distance, (unsigned)trace[index].confidence,
            (unsigned)trace[index].flags);
    }
    (void)fprintf(stderr, "\n");
}

static size_t shrink_trace(uint32_t seed, model_cs_event_t *trace, size_t count)
{
    size_t chunk = count / 2U;
    while (chunk > 0U) {
        bool reduced = false;
        for (size_t start = 0U; start + chunk <= count; ++start) {
            model_cs_event_t candidate[MODEL_MAX_TRACE];
            size_t failed_at = 0U;
            memcpy(candidate, trace, start * sizeof(trace[0]));
            memcpy(&candidate[start], &trace[start + chunk], (count - start - chunk) * sizeof(trace[0]));
            if (!run_cs_trace(candidate, count - chunk, &failed_at)) {
                memcpy(trace, candidate, (count - chunk) * sizeof(trace[0]));
                count -= chunk; reduced = true; break;
            }
        }
        if (!reduced) chunk /= 2U;
    }
    print_trace(seed, trace, count, "shrunk");
    return count;
}

/* characterization: fixed-seed, bounded state/event replay prints and chunk-shrinks any counterexample. */
static void test_seeded_cs_reference_replay(void)
{
    static const uint32_t seeds[] = {0x00000001U, 0x0000C0FFU, 0x000051A7U, 0xFFFFFFFFU};
    for (size_t seed_index = 0U; seed_index < sizeof(seeds) / sizeof(seeds[0]); ++seed_index) {
        uint32_t random = seeds[seed_index];
        for (size_t index = 0U; index < MODEL_MAX_TRACE; ++index) s_trace[index] = make_seeded_event(&random, index);
        s_trace[0] = (model_cs_event_t){.kind=M_EV_ASSIGN,.node=2U};
        s_trace[1] = (model_cs_event_t){.kind=M_EV_ACCEPT,.request=make_model_request(1U, 1U, 1U)};
        s_trace[2] = (model_cs_event_t){.kind=M_EV_ACCEPT,.request=make_model_request(1U, 1U, 1U)};
        s_trace[3] = (model_cs_event_t){.kind=M_EV_RECORD_ENQUEUE,.request=make_model_request(2U, 2U, 1U),.command=M_CS_BUSY};
        s_trace[4] = (model_cs_event_t){.kind=M_EV_SELECT,.request=make_model_request(2U, 2U, 1U)};
        s_trace[5] = (model_cs_event_t){.kind=M_EV_TAKE};
        s_trace[6] = (model_cs_event_t){.kind=M_EV_COMPLETE,.step=make_model_request(1U, 1U, 1U).step,
            .outcome=M_OUTCOME_SUCCESS,.distance=2222,.confidence=900U};
        s_trace[7] = (model_cs_event_t){.kind=M_EV_SELECT,.request=make_model_request(1U, 2U, 1U)};
        s_trace[8] = (model_cs_event_t){.kind=M_EV_TAKE};
        s_trace[9] = (model_cs_event_t){.kind=M_EV_SELECT,.request=make_model_request(1U, 1U, 1U)};
        s_trace[10] = (model_cs_event_t){.kind=M_EV_TAKE};
        s_trace[11] = (model_cs_event_t){.kind=M_EV_COMPLETE,.step=make_model_request(1U, 1U, 1U).step,.outcome=M_OUTCOME_TIMEOUT};
        size_t failed_at = 0U;
        if (!run_cs_trace(s_trace, MODEL_MAX_TRACE, &failed_at)) {
            print_trace(seeds[seed_index], s_trace, MODEL_MAX_TRACE, "failure");
            const size_t shrunk = shrink_trace(seeds[seed_index], s_trace, MODEL_MAX_TRACE);
            (void)fprintf(stderr, "seed=0x%08X mismatch_at=%zu minimized=%zu\n",
                (unsigned)seeds[seed_index], failed_at, shrunk);
            assert(false && "seeded CS reference-model mismatch");
        }
    }
}

/* characterization: entry point for independent node, ASSIGN, sensor, and CS models. */
int main(void)
{
    test_node_transition_truth_table();
    test_bootstrap_assign_model();
    test_sensor_session_model();
    test_seeded_cs_reference_replay();
    return 0;
}
