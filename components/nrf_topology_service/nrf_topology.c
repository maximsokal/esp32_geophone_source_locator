#include "nrf_topology.h"
#include "nrf_topology_periods.h"
#include "sensor_bus.h"

#include <stddef.h>
#include <string.h>

typedef enum {
    ASSIGN_CONFIRM_OK = 0,
    ASSIGN_CONFIRM_CONFLICT,
    ASSIGN_CONFIRM_PROTOCOL_ERROR,
    ASSIGN_CONFIRM_TRANSPORT_ERROR,
    ASSIGN_CONFIRM_BUS_BUSY,
} assign_confirm_result_t;

typedef struct {
    bool safe_for_remotes;
    bool assigned;
} main_reconcile_result_t;

static void node_snapshot_reset(nrf_topology_node_snapshot_t *node)
{
    memset(node, 0, sizeof(*node));
    node->state = NRF_TOPOLOGY_NODE_UNKNOWN;
    node->node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    node->mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
}

static void snapshot_reset(nrf_topology_snapshot_t *snapshot, uint32_t generation)
{
    size_t index;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->generation = generation;
    node_snapshot_reset(&snapshot->main);
    for (index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        node_snapshot_reset(&snapshot->remotes[index]);
    }
}

static bool hardware_id_equal_node(
    const nrf_topology_node_snapshot_t *left,
    const nrf_topology_node_snapshot_t *right)
{
    return left->hardware_id_length == right->hardware_id_length &&
           left->hardware_id_length > 0U &&
           memcmp(left->hardware_id, right->hardware_id, left->hardware_id_length) == 0;
}

static void node_from_status(
    nrf_topology_node_snapshot_t *node,
    nrf_topology_node_state_t state,
    const nrf_upstream_status_t *status)
{
    node_snapshot_reset(node);
    node->state = state;
    if (status == NULL) {
        return;
    }
    node->hardware_id_length = status->hardware_id_length;
    memcpy(node->hardware_id, status->hardware_id, NRF_UPSTREAM_PROTOCOL_HW_ID_MAX);
    node->node_id = status->node_id;
    node->mux_channel = status->mux_channel;
    node->current_address = status->target_address;
    node->protocol_result = status->result;
    node->assignment_request_id = status->request_id;
    node->role = status->role;
    node->lifecycle = status->lifecycle;
    node->flags = status->flags;
}

static void conflict_from_status(
    nrf_topology_node_snapshot_t *node,
    nrf_topology_conflict_reason_t reason,
    const nrf_upstream_status_t *status)
{
    node_from_status(node, NRF_TOPOLOGY_NODE_CONFLICT, status);
    node->conflict_reason = (uint8_t)reason;
}

static bool status_is_registered_ready(const nrf_upstream_status_t *status, uint8_t address)
{
    return status->lifecycle == NRF_UPSTREAM_LIFECYCLE_READY &&
           status->target_address == address &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED) != 0U;
}

static bool status_is_unassigned_discovery(const nrf_upstream_status_t *status)
{
    return status_is_registered_ready(status, NRF_UPSTREAM_ADDRESS_DISCOVERY) &&
           status->role == NRF_UPSTREAM_ROLE_UNASSIGNED &&
           status->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED &&
           status->mux_channel == NRF_UPSTREAM_MUX_CHANNEL_NONE &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) == 0U;
}

/*
 * MAIN identity only. status->result is the outcome of the last request the
 * node saw, not part of who it is: the nRF keeps a rejected or unsupported
 * request's result in STATUS until the next ASSIGN completes. Requiring
 * RESULT_OK here turned one stray bad frame into a permanent CONFLICT that
 * hid every REMOTE. The result stays visible in node->protocol_result.
 */
static bool status_is_main(const nrf_upstream_status_t *status)
{
    return status_is_registered_ready(status, NRF_UPSTREAM_ADDRESS_MAIN) &&
           status->role == NRF_UPSTREAM_ROLE_MAIN &&
           status->node_id == NRF_TOPOLOGY_MAIN_NODE_ID &&
           status->mux_channel == NRF_UPSTREAM_MUX_CHANNEL_NONE &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U;
}

/*
 * REMOTE identity only, for the same reason as status_is_main(): status->result
 * is the outcome of the last request (BUSY, PENDING, INVALID_FRAME,
 * UNSUPPORTED_TYPE, ...) and is not part of who the node is. Wrong role,
 * node_id, mux channel, address or a missing ASSIGNED flag are still conflicts;
 * duplicate hardware ids are caught in detect_duplicate_hardware_ids().
 */
static bool status_is_remote(const nrf_upstream_status_t *status, uint8_t channel)
{
    return status_is_registered_ready(status, NRF_UPSTREAM_ADDRESS_REMOTE) &&
           status->role == NRF_UPSTREAM_ROLE_REMOTE &&
           status->node_id == (uint8_t)(channel + 1U) &&
           status->mux_channel == channel &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U;
}

static bool topology_abort_requested(const nrf_topology_io_t *io)
{
    return io->abort_requested != NULL && io->abort_requested(io->context);
}

static uint32_t next_request_id(nrf_topology_controller_t *controller)
{
    uint32_t id = controller->next_request_id;
    if (id == 0U) {
        id = 1U;
    }
    controller->next_request_id = id + 1U;
    if (controller->next_request_id == 0U) {
        controller->next_request_id = 1U;
    }
    return id;
}

static bool bus_yield_error(esp_err_t err)
{
    return err == SENSOR_BUS_ERR_MUTEX_BUSY ||
        err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET;
}

static nrf_topology_node_state_t state_for_read_error(esp_err_t err)
{
    if (bus_yield_error(err)) {
        return NRF_TOPOLOGY_NODE_UNKNOWN;
    }
    return err == ESP_ERR_INVALID_RESPONSE
        ? NRF_TOPOLOGY_NODE_PROTOCOL_ERROR
        : NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
}

static esp_err_t probe_status(
    const nrf_topology_io_t *io,
    uint8_t address,
    bool *present,
    nrf_upstream_status_t *status,
    nrf_topology_node_state_t *error_state,
    bool *bus_busy)
{
    esp_err_t err;
    memset(status, 0, sizeof(*status));
    *present = false;
    *error_state = NRF_TOPOLOGY_NODE_UNKNOWN;
    err = io->read_status(io->context, address, status);
    if (err == ESP_OK) {
        *present = true;
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    *error_state = state_for_read_error(err);
    if (bus_yield_error(err) && bus_busy != NULL) {
        *bus_busy = true;
    }
    return err;
}

static assign_confirm_result_t assign_error_result(esp_err_t err, bool *bus_busy)
{
    if (bus_yield_error(err)) {
        if (bus_busy != NULL) {
            *bus_busy = true;
        }
        return ASSIGN_CONFIRM_BUS_BUSY;
    }
    return err == ESP_ERR_INVALID_RESPONSE
        ? ASSIGN_CONFIRM_PROTOCOL_ERROR
        : ASSIGN_CONFIRM_TRANSPORT_ERROR;
}

static bool status_matches_assignment(
    const nrf_upstream_status_t *status,
    const nrf_upstream_assign_t *assign,
    uint8_t runtime_address)
{
    return status->request_id == assign->request_id &&
           status->hardware_id_length == assign->hardware_id_length &&
           memcmp(status->hardware_id, assign->hardware_id, assign->hardware_id_length) == 0 &&
           status->role == assign->role &&
           status->node_id == assign->node_id &&
           status->mux_channel == assign->mux_channel &&
           status->target_address == runtime_address &&
           status->lifecycle == NRF_UPSTREAM_LIFECYCLE_READY &&
           status->result == NRF_UPSTREAM_RESULT_OK &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED) != 0U;
}

static bool status_same_hardware(
    const nrf_upstream_status_t *status,
    const nrf_upstream_assign_t *assign)
{
    return status->hardware_id_length == assign->hardware_id_length &&
           memcmp(status->hardware_id, assign->hardware_id, assign->hardware_id_length) == 0;
}

static assign_confirm_result_t assign_and_confirm(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    bool *bus_busy,
    const nrf_upstream_status_t *discovery,
    uint8_t role,
    uint8_t node_id,
    uint8_t mux_channel,
    nrf_upstream_status_t *confirmed)
{
    nrf_upstream_assign_t assign;
    bool retry_used = false;
    const uint8_t runtime_address = role == NRF_UPSTREAM_ROLE_MAIN
        ? NRF_UPSTREAM_ADDRESS_MAIN
        : NRF_UPSTREAM_ADDRESS_REMOTE;

    memset(&assign, 0, sizeof(assign));
    assign.request_id = next_request_id(controller);
    assign.hardware_id_length = discovery->hardware_id_length;
    memcpy(assign.hardware_id, discovery->hardware_id, NRF_UPSTREAM_PROTOCOL_HW_ID_MAX);
    assign.role = role;
    assign.node_id = node_id;
    assign.mux_channel = mux_channel;

    if (topology_abort_requested(io)) {
        return ASSIGN_CONFIRM_TRANSPORT_ERROR;
    }
    esp_err_t assign_err = io->write_assign(
        io->context, NRF_UPSTREAM_ADDRESS_DISCOVERY, &assign);
    if (assign_err != ESP_OK) {
        return assign_error_result(assign_err, bus_busy);
    }
    const uint64_t started_ms = io->now_ms(io->context);

    for (;;) {
        if (topology_abort_requested(io) ||
            io->now_ms(io->context) - started_ms >= controller->config.confirm_timeout_ms) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
        nrf_upstream_status_t runtime_status;
        nrf_upstream_status_t discovery_status;
        esp_err_t err;

        memset(&runtime_status, 0, sizeof(runtime_status));
        err = io->read_status(io->context, runtime_address, &runtime_status);
        if (err == ESP_OK) {
            if (!status_same_hardware(&runtime_status, &assign)) {
                return ASSIGN_CONFIRM_CONFLICT;
            }
            if (status_matches_assignment(&runtime_status, &assign, runtime_address)) {
                *confirmed = runtime_status;
                return ASSIGN_CONFIRM_OK;
            }
            if (runtime_status.request_id != assign.request_id ||
                runtime_status.result != NRF_UPSTREAM_RESULT_PENDING) {
                return ASSIGN_CONFIRM_CONFLICT;
            }
        }
        else if (err != ESP_ERR_NOT_FOUND) {
            return assign_error_result(err, bus_busy);
        }

        if (io->now_ms(io->context) - started_ms >= controller->config.confirm_timeout_ms) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
        memset(&discovery_status, 0, sizeof(discovery_status));
        err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_DISCOVERY, &discovery_status);
        if (err == ESP_OK) {
            if (!status_same_hardware(&discovery_status, &assign)) {
                return ASSIGN_CONFIRM_CONFLICT;
            }
            if (!status_is_unassigned_discovery(&discovery_status)) {
                return ASSIGN_CONFIRM_PROTOCOL_ERROR;
            }
            if (discovery_status.request_id == assign.request_id) {
                if (discovery_status.result == NRF_UPSTREAM_RESULT_PENDING) {
                    /* Worker has accepted the request but has not committed it yet. */
                }
                else if (discovery_status.result == NRF_UPSTREAM_RESULT_READDRESS_FAILED) {
                    if (retry_used) {
                        return ASSIGN_CONFIRM_TRANSPORT_ERROR;
                    }
                    retry_used = true;
                    assign_err = io->write_assign(
                        io->context, NRF_UPSTREAM_ADDRESS_DISCOVERY, &assign);
                    if (assign_err != ESP_OK) {
                        return assign_error_result(assign_err, bus_busy);
                    }
                }
                else if (discovery_status.result != NRF_UPSTREAM_RESULT_OK) {
                    return ASSIGN_CONFIRM_PROTOCOL_ERROR;
                }
            }
            else {
                /*
                 * The published STATUS can still be the pre-request snapshot
                 * until the deferred nRF worker runs. It can also be request 0
                 * after a cold reboot. Do not enqueue a speculative duplicate;
                 * keep the bounded poll and let the next reconcile restart a
                 * rebooted node cleanly if this request never becomes visible.
                 */
            }
        }
        else if (err != ESP_ERR_NOT_FOUND) {
            return assign_error_result(err, bus_busy);
        }

        const uint64_t elapsed_ms = io->now_ms(io->context) - started_ms;
        if (elapsed_ms >= controller->config.confirm_timeout_ms) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
        const uint32_t remaining_ms = controller->config.confirm_timeout_ms - (uint32_t)elapsed_ms;
        io->sleep_ms(io->context, remaining_ms < controller->config.poll_interval_ms
            ? remaining_ms : controller->config.poll_interval_ms);
        if (topology_abort_requested(io)) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
    }
}

static nrf_topology_node_state_t node_state_for_assign_result(assign_confirm_result_t result)
{
    switch (result) {
    case ASSIGN_CONFIRM_CONFLICT:
        return NRF_TOPOLOGY_NODE_CONFLICT;
    case ASSIGN_CONFIRM_PROTOCOL_ERROR:
        return NRF_TOPOLOGY_NODE_PROTOCOL_ERROR;
    case ASSIGN_CONFIRM_BUS_BUSY:
        return NRF_TOPOLOGY_NODE_UNKNOWN;
    case ASSIGN_CONFIRM_TRANSPORT_ERROR:
    default:
        return NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
    }
}

static main_reconcile_result_t reconcile_main(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot)
{
    bool discovery_present = false;
    bool main_present = false;
    bool remote_present = false;
    nrf_upstream_status_t discovery_status;
    nrf_upstream_status_t main_status;
    nrf_upstream_status_t remote_status;
    nrf_topology_node_state_t read_error = NRF_TOPOLOGY_NODE_UNKNOWN;
    main_reconcile_result_t result = {0};

    if (probe_status(io, NRF_UPSTREAM_ADDRESS_DISCOVERY, &discovery_present,
                     &discovery_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_MAIN, &main_present,
                     &main_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_REMOTE, &remote_present,
                     &remote_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }


    if (remote_present) {
        conflict_from_status(
            &snapshot->main, NRF_TOPOLOGY_CONFLICT_REMOTE_ON_DIRECT_BUS, &remote_status);
        return result;
    }
    if (discovery_present && main_present) {
        conflict_from_status(
            &snapshot->main, NRF_TOPOLOGY_CONFLICT_DISCOVERY_AND_MAIN, &main_status);
        return result;
    }
    if (main_present) {
        if (!status_is_main(&main_status)) {
            conflict_from_status(
                &snapshot->main, NRF_TOPOLOGY_CONFLICT_MAIN_IDENTITY, &main_status);
            return result;
        }
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_ASSIGNED, &main_status);
        result.safe_for_remotes = true;
        result.assigned = true;
        return result;
    }
    if (!discovery_present) {
        snapshot->main.state = NRF_TOPOLOGY_NODE_ABSENT;
        result.safe_for_remotes = true;
        return result;
    }

    if (!status_is_unassigned_discovery(&discovery_status)) {
        conflict_from_status(
            &snapshot->main, NRF_TOPOLOGY_CONFLICT_DISCOVERY_NOT_UNASSIGNED, &discovery_status);
        return result;
    }

    nrf_upstream_status_t confirmed;
    const assign_confirm_result_t assign_result = assign_and_confirm(
        controller,
        io,
        &snapshot->bus_busy,
        &discovery_status,
        NRF_UPSTREAM_ROLE_MAIN,
        NRF_TOPOLOGY_MAIN_NODE_ID,
        NRF_UPSTREAM_MUX_CHANNEL_NONE,
        &confirmed);
    if (assign_result == ASSIGN_CONFIRM_OK) {
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_ASSIGNED, &confirmed);
        result.safe_for_remotes = true;
        result.assigned = true;
    }
    else {
        node_from_status(&snapshot->main, node_state_for_assign_result(assign_result), &discovery_status);
    }
    return result;
}

/*
 * Used whenever the mux state is unknown (discovery fault, or a close that
 * could not be confirmed). No channel is opened and 0x30 is neither read
 * nor written (a remote's discovery node may be visible through an open
 * channel). MAIN, however, sits directly on the ESP bus and only ever answers
 * at 0x31: a read-only status there cannot collide with anything, so a mux
 * fault does not hide a healthy MAIN. An absent or unreadable 0x31 keeps the
 * conservative TRANSPORT_ERROR already stored in snapshot->main.
 */
static void report_main_status_only(
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot)
{
    bool main_present = false;
    nrf_upstream_status_t main_status;
    nrf_topology_node_state_t read_error = NRF_TOPOLOGY_NODE_UNKNOWN;

    if (probe_status(io, NRF_UPSTREAM_ADDRESS_MAIN, &main_present,
                     &main_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        snapshot->main.state = read_error;
        return;
    }
    if (!main_present) {
        return;
    }
    if (status_is_main(&main_status)) {
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_ASSIGNED, &main_status);
    }
    else {
        conflict_from_status(
            &snapshot->main, NRF_TOPOLOGY_CONFLICT_MAIN_IDENTITY, &main_status);
    }
}

typedef enum {
    MAIN_VERIFY_OK,
    MAIN_VERIFY_LOST,            /* answers as something else, or does not answer at all */
    MAIN_VERIFY_TRANSPORT_ERROR, /* the read itself failed twice */
    MAIN_VERIFY_BUSY,
} main_verify_t;

/*
 * One failed read with a channel open is not "MAIN lost": the shared bus
 * times out now and then (the nRF target answers late), so a transport error
 * is read once more before it is reported, and it is reported as an error of
 * the transport. Only a MAIN that does not ACK or answers with another
 * identity counts as lost.
 */
static main_verify_t verify_known_main(
    const nrf_topology_io_t *io,
    const nrf_topology_node_snapshot_t *known_main,
    bool *bus_busy)
{
    nrf_upstream_status_t status;
    esp_err_t err = ESP_FAIL;

    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_MAIN, &status);
        if (bus_yield_error(err)) {
            *bus_busy = true;
            return MAIN_VERIFY_BUSY;
        }
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            break;
        }
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return MAIN_VERIFY_LOST;
    }
    if (err != ESP_OK) {
        return MAIN_VERIFY_TRANSPORT_ERROR;
    }
    if (!status_is_main(&status) ||
        status.hardware_id_length != known_main->hardware_id_length ||
        memcmp(status.hardware_id, known_main->hardware_id, status.hardware_id_length) != 0) {
        return MAIN_VERIFY_LOST;
    }
    return MAIN_VERIFY_OK;
}

uint64_t nrf_topology_channel_quarantine_remaining_ms(
    const nrf_topology_controller_t *controller,
    uint8_t channel,
    uint64_t now_ms)
{
    if (controller == NULL || channel >= NRF_TOPOLOGY_REMOTE_COUNT ||
        controller->channel_quarantined_until_ms[channel] <= now_ms) {
        return 0U;
    }
    return controller->channel_quarantined_until_ms[channel] - now_ms;
}

/* See NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS. */
static void channel_select_failed(
    nrf_topology_controller_t *controller,
    uint8_t channel,
    uint64_t now_ms)
{
    if (controller->channel_select_failures[channel] < UINT8_MAX) {
        controller->channel_select_failures[channel]++;
    }
    const uint8_t threshold = controller->remote_confirmed_valid[channel] ? 2U : 1U;
    const uint8_t failures = controller->channel_select_failures[channel];
    if (failures < threshold) {
        return;
    }
    const unsigned doublings = (unsigned)(failures - threshold);
    uint64_t quarantine_ms = NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS;
    if (doublings < 16U &&
        ((uint64_t)NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS << doublings) <
            NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS) {
        quarantine_ms = (uint64_t)NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS << doublings;
    }
    /* Remember whether a node is being cut off. The answer is kept until a select succeeds, because the
     * retained identity expires long before a long quarantine does. */
    if (controller->remote_confirmed_valid[channel]) {
        controller->channel_quarantined_with_node[channel] = true;
    }
    controller->channel_quarantined_until_ms[channel] = now_ms + quarantine_ms;
}

static void channel_select_succeeded(nrf_topology_controller_t *controller, uint8_t channel)
{
    controller->channel_select_failures[channel] = 0U;
    controller->channel_quarantined_until_ms[channel] = 0U;
    controller->channel_quarantined_with_node[channel] = false;
}

static bool close_mux_or_fault(const nrf_topology_io_t *io, nrf_topology_snapshot_t *snapshot)
{
    if (!io->tca_available) {
        return true;
    }
    const esp_err_t err = io->mux_close(io->context);
    if (bus_yield_error(err)) {
        snapshot->bus_busy = true;
        return true;
    }
    if (err != ESP_OK) {
        snapshot->tca_fault = true;
        return false;
    }
    return true;
}

static void scan_remote_channel(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot,
    uint8_t channel,
    bool main_assigned,
    bool *main_lost,
    bool *continue_scan)
{
    nrf_topology_node_snapshot_t *node = &snapshot->remotes[channel];
    bool discovery_present = false;
    bool remote_present = false;
    nrf_upstream_status_t discovery_status;
    nrf_upstream_status_t remote_status;
    nrf_topology_node_state_t read_error = NRF_TOPOLOGY_NODE_UNKNOWN;

    const esp_err_t select_err = io->mux_select(io->context, channel);
    if (select_err != ESP_OK) {
        if (bus_yield_error(select_err)) {
            snapshot->bus_busy = true;
            node->state = NRF_TOPOLOGY_NODE_UNKNOWN;
            *continue_scan = false;
            return;
        }
        /* The select itself failed: this socket may be holding the bus. Do not come back to it at once. */
        channel_select_failed(controller, channel, io->now_ms(io->context));
        node->state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        if (!close_mux_or_fault(io, snapshot) || snapshot->bus_busy) {
            *continue_scan = false;
        }
        return;
    }
    channel_select_succeeded(controller, channel);

    const main_verify_t main_verdict = main_assigned
        ? verify_known_main(io, &snapshot->main, &snapshot->bus_busy)
        : MAIN_VERIFY_OK;
    if (main_verdict == MAIN_VERIFY_BUSY) {
        node->state = NRF_TOPOLOGY_NODE_UNKNOWN;
        (void)close_mux_or_fault(io, snapshot);
        *continue_scan = false;
        return;
    }
    if (main_verdict == MAIN_VERIFY_TRANSPORT_ERROR) {
        node->state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        (void)close_mux_or_fault(io, snapshot);
        *continue_scan = false;
        return;
    }
    if (main_verdict == MAIN_VERIFY_LOST) {
        node->state = NRF_TOPOLOGY_NODE_CONFLICT;
        node->conflict_reason = (uint8_t)NRF_TOPOLOGY_CONFLICT_MAIN_LOST_DURING_SCAN;
        *main_lost = true;
        (void)close_mux_or_fault(io, snapshot);
        *continue_scan = false;
        return;
    }

    if (probe_status(io, NRF_UPSTREAM_ADDRESS_DISCOVERY, &discovery_present,
                     &discovery_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        node->state = read_error;
        if (!close_mux_or_fault(io, snapshot) || snapshot->bus_busy ||
            read_error == NRF_TOPOLOGY_NODE_UNKNOWN) {
            *continue_scan = false;
        }
        return;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_REMOTE, &remote_present,
                     &remote_status, &read_error, &snapshot->bus_busy) != ESP_OK) {
        node->state = read_error;
        if (!close_mux_or_fault(io, snapshot) || snapshot->bus_busy ||
            read_error == NRF_TOPOLOGY_NODE_UNKNOWN) {
            *continue_scan = false;
        }
        return;
    }

    if (discovery_present && remote_present) {
        conflict_from_status(
            node, NRF_TOPOLOGY_CONFLICT_REMOTE_DISCOVERY_AND_RUNTIME, &remote_status);
    }
    else if (remote_present) {
        if (status_is_remote(&remote_status, channel)) {
            node_from_status(node, NRF_TOPOLOGY_NODE_ASSIGNED, &remote_status);
        }
        else {
            conflict_from_status(
                node, NRF_TOPOLOGY_CONFLICT_REMOTE_IDENTITY, &remote_status);
        }
    }
    else if (discovery_present) {
        if (!status_is_unassigned_discovery(&discovery_status)) {
            conflict_from_status(
                node, NRF_TOPOLOGY_CONFLICT_REMOTE_DISCOVERY_NOT_UNASSIGNED, &discovery_status);
        }
        else {
            nrf_upstream_status_t confirmed;
            const assign_confirm_result_t assign_result = assign_and_confirm(
                controller,
                io,
                &snapshot->bus_busy,
                &discovery_status,
                NRF_UPSTREAM_ROLE_REMOTE,
                (uint8_t)(channel + 1U),
                channel,
                &confirmed);
            if (assign_result == ASSIGN_CONFIRM_OK) {
                node_from_status(node, NRF_TOPOLOGY_NODE_ASSIGNED, &confirmed);
            }
            else {
                node_from_status(node, node_state_for_assign_result(assign_result), &discovery_status);
            }
        }
    }
    else {
        node->state = NRF_TOPOLOGY_NODE_ABSENT;
    }

    if (!close_mux_or_fault(io, snapshot) || snapshot->bus_busy) {
        *continue_scan = false;
    }
}

static void detect_duplicate_hardware_ids(nrf_topology_snapshot_t *snapshot)
{
    size_t i;
    size_t j;
    nrf_topology_node_snapshot_t *nodes[NRF_TOPOLOGY_REMOTE_COUNT + 1U];
    nodes[0] = &snapshot->main;
    for (i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        nodes[i + 1U] = &snapshot->remotes[i];
    }
    for (i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT + 1U; ++i) {
        if (nodes[i]->state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        for (j = i + 1U; j < NRF_TOPOLOGY_REMOTE_COUNT + 1U; ++j) {
            if (nodes[j]->state == NRF_TOPOLOGY_NODE_ASSIGNED &&
                hardware_id_equal_node(nodes[i], nodes[j])) {
                nodes[i]->state = NRF_TOPOLOGY_NODE_CONFLICT;
                nodes[j]->state = NRF_TOPOLOGY_NODE_CONFLICT;
                nodes[i]->conflict_reason = (uint8_t)NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID;
                nodes[j]->conflict_reason = (uint8_t)NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID;
            }
        }
    }
}

/*
 * A transport/protocol read error is not evidence that an assigned remote
 * disappeared. Hold its last verified identity for the same bounded window
 * used for a globally stale topology. A confirmed absence or identity
 * conflict must never be hidden by a cached ASSIGNED identity.
 *
 * Only a successful, fresh ASSIGNED scan advances the window: replaying a
 * cached identity must not make an absent remote permanently present.
 */
static void reconcile_transient_remote_failures(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot)
{
    bool retained[NRF_TOPOLOGY_REMOTE_COUNT] = {false};
    const uint64_t now_ms = io->now_ms(io->context);

    /* Bus ownership and mux-close failures are handled by the global stale
     * policy. Do not turn an uncertain mux routing into an apparent ACK. */
    if (snapshot->bus_busy || snapshot->tca_fault) {
        detect_duplicate_hardware_ids(snapshot);
        return;
    }

    /*
     * A channel left UNKNOWN because the scan was cut short is only kept when MAIN's own state was settled this pass:
     * ASSIGNED, or confirmed ABSENT (REMOTE_ONLY). A MAIN read that failed leaves the channels unscanned for a reason that
     * says nothing about them, so they are not revived. Retention stays bounded by NRF_TOPOLOGY_STALE_LIMIT_MS below.
     */
    const bool main_settled = snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED ||
        snapshot->main.state == NRF_TOPOLOGY_NODE_ABSENT;
    for (size_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        nrf_topology_node_snapshot_t *const remote = &snapshot->remotes[i];
        if ((remote->state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR ||
             remote->state == NRF_TOPOLOGY_NODE_PROTOCOL_ERROR ||
             (remote->state == NRF_TOPOLOGY_NODE_UNKNOWN && main_settled)) &&
            controller->remote_confirmed_valid[i] &&
            now_ms >= controller->remote_confirmed_at_ms[i] &&
            now_ms - controller->remote_confirmed_at_ms[i] < NRF_TOPOLOGY_STALE_LIMIT_MS) {
            *remote = controller->last_confirmed_remotes[i];
            retained[i] = true;
        }
    }

    /* Restoring a cached node must not conceal duplicate hardware identities. */
    detect_duplicate_hardware_ids(snapshot);

    for (size_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        const nrf_topology_node_snapshot_t *const remote = &snapshot->remotes[i];
        if (remote->state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            if (!retained[i]) {
                controller->last_confirmed_remotes[i] = *remote;
                controller->remote_confirmed_at_ms[i] = now_ms;
                controller->remote_confirmed_valid[i] = true;
            }
        } else if (remote->state == NRF_TOPOLOGY_NODE_ABSENT ||
                   remote->state == NRF_TOPOLOGY_NODE_CONFLICT) {
            controller->remote_confirmed_valid[i] = false;
        } else if (controller->remote_confirmed_valid[i] &&
                   (now_ms < controller->remote_confirmed_at_ms[i] ||
                    now_ms - controller->remote_confirmed_at_ms[i] >= NRF_TOPOLOGY_STALE_LIMIT_MS)) {
            controller->remote_confirmed_valid[i] = false;
        }
    }
}

esp_err_t nrf_topology_controller_init(
    nrf_topology_controller_t *controller,
    const nrf_topology_config_t *config)
{
    if (controller == NULL || config == NULL ||
        config->confirm_timeout_ms == 0U || config->poll_interval_ms == 0U ||
        config->poll_interval_ms > config->confirm_timeout_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(controller, 0, sizeof(*controller));
    controller->config = *config;
    controller->next_request_id = config->request_seed == 0U ? 1U : config->request_seed;
    return ESP_OK;
}

esp_err_t nrf_topology_reconcile(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot)
{
    main_reconcile_result_t main_result;
    size_t channel;
    bool continue_scan = true;
    bool main_lost = false;

    if (controller == NULL || io == NULL || snapshot == NULL ||
        io->read_status == NULL || io->write_assign == NULL || io->sleep_ms == NULL ||
        io->now_ms == NULL ||
        (io->tca_available && (io->mux_close == NULL || io->mux_select == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }

    controller->generation++;
    if (controller->generation == 0U) {
        controller->generation = 1U;
    }
    snapshot_reset(snapshot, controller->generation);
    snapshot->tca_available = io->tca_available;
    snapshot->tca_fault = io->tca_fault;

    if (io->tca_fault) {
        for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        }
        snapshot->main.state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        report_main_status_only(io, snapshot);
        return ESP_OK;
    }

    if (io->tca_available && !close_mux_or_fault(io, snapshot)) {
        /* The mux could not be forced closed, so it may still be open. */
        snapshot->main.state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        }
        report_main_status_only(io, snapshot);
        return ESP_OK;
    }
    if (snapshot->bus_busy) {
        return ESP_OK;
    }

    main_result = reconcile_main(controller, io, snapshot);
    if (!io->tca_available) {
        for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_ABSENT;
        }
        detect_duplicate_hardware_ids(snapshot);
        return ESP_OK;
    }

    if (!main_result.safe_for_remotes) {
        /* An unassigned/conflicted direct node may still own 0x30. Never open a channel. */
        return ESP_OK;
    }

    for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT && continue_scan; ++channel) {
        if (topology_abort_requested(io)) {
            break;
        }
        if (nrf_topology_channel_quarantine_remaining_ms(
                controller, (uint8_t)channel, io->now_ms(io->context)) != 0U) {
            /*
             * Not selected, so nothing is known about it in this pass. A channel that had a confirmed node is a
             * transport error (its identity is retained for the usual bounded window); a channel that never had
             * one is reported as an empty socket, which is what it is for every consumer.
             */
            snapshot->quarantined_channels |= (uint8_t)(1U << channel);
            snapshot->remotes[channel].state = controller->channel_quarantined_with_node[channel]
                ? NRF_TOPOLOGY_NODE_TRANSPORT_ERROR
                : NRF_TOPOLOGY_NODE_ABSENT;
            continue;
        }
        if (!main_result.assigned && snapshot->main.state == NRF_TOPOLOGY_NODE_ABSENT) {
            /*
             * MAIN may be hot-plugged or reboot into discovery while remotes
             * are being enumerated. Re-check it with the mux still closed so
             * we never knowingly open a channel onto a newly occupied 0x30.
             */
            main_result = reconcile_main(controller, io, snapshot);
            if (!main_result.safe_for_remotes) {
                break;
            }
        }
        scan_remote_channel(
            controller,
            io,
            snapshot,
            (uint8_t)channel,
            main_result.assigned,
            &main_lost,
            &continue_scan);
        if (main_lost) {
            /* Close was attempted by scan_remote_channel. Reconcile direct MAIN before any new 0x30 access. */
            main_result = reconcile_main(controller, io, snapshot);
            (void)main_result;
            break;
        }
    }

    if (!snapshot->tca_fault) {
        (void)close_mux_or_fault(io, snapshot);
    }
    reconcile_transient_remote_failures(controller, io, snapshot);
    return ESP_OK;
}
