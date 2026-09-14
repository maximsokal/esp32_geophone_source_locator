#include "nrf_topology.h"

#include <stddef.h>
#include <string.h>

typedef enum {
    ASSIGN_CONFIRM_OK = 0,
    ASSIGN_CONFIRM_CONFLICT,
    ASSIGN_CONFIRM_PROTOCOL_ERROR,
    ASSIGN_CONFIRM_TRANSPORT_ERROR,
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

static bool status_is_main(const nrf_upstream_status_t *status)
{
    return status_is_registered_ready(status, NRF_UPSTREAM_ADDRESS_MAIN) &&
           status->role == NRF_UPSTREAM_ROLE_MAIN &&
           status->node_id == NRF_TOPOLOGY_MAIN_NODE_ID &&
           status->mux_channel == NRF_UPSTREAM_MUX_CHANNEL_NONE &&
           status->result == NRF_UPSTREAM_RESULT_OK &&
           (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U;
}

static bool status_is_remote(const nrf_upstream_status_t *status, uint8_t channel)
{
    return status_is_registered_ready(status, NRF_UPSTREAM_ADDRESS_REMOTE) &&
           status->role == NRF_UPSTREAM_ROLE_REMOTE &&
           status->node_id == (uint8_t)(channel + 1U) &&
           status->mux_channel == channel &&
           status->result == NRF_UPSTREAM_RESULT_OK &&
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

static nrf_topology_node_state_t state_for_read_error(esp_err_t err)
{
    return err == ESP_ERR_INVALID_RESPONSE
        ? NRF_TOPOLOGY_NODE_PROTOCOL_ERROR
        : NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
}

static esp_err_t probe_status(
    const nrf_topology_io_t *io,
    uint8_t address,
    bool *present,
    nrf_upstream_status_t *status,
    nrf_topology_node_state_t *error_state)
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
    return err;
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
    const nrf_upstream_status_t *discovery,
    uint8_t role,
    uint8_t node_id,
    uint8_t mux_channel,
    nrf_upstream_status_t *confirmed)
{
    nrf_upstream_assign_t assign;
    uint32_t elapsed_ms = 0U;
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
    if (io->write_assign(io->context, NRF_UPSTREAM_ADDRESS_DISCOVERY, &assign) != ESP_OK) {
        return ASSIGN_CONFIRM_TRANSPORT_ERROR;
    }

    for (;;) {
        if (topology_abort_requested(io)) {
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
            return err == ESP_ERR_INVALID_RESPONSE
                ? ASSIGN_CONFIRM_PROTOCOL_ERROR
                : ASSIGN_CONFIRM_TRANSPORT_ERROR;
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
                    if (io->write_assign(io->context, NRF_UPSTREAM_ADDRESS_DISCOVERY, &assign) != ESP_OK) {
                        return ASSIGN_CONFIRM_TRANSPORT_ERROR;
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
            return err == ESP_ERR_INVALID_RESPONSE
                ? ASSIGN_CONFIRM_PROTOCOL_ERROR
                : ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }

        if (elapsed_ms >= controller->config.confirm_timeout_ms) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
        io->sleep_ms(io->context, controller->config.poll_interval_ms);
        if (topology_abort_requested(io)) {
            return ASSIGN_CONFIRM_TRANSPORT_ERROR;
        }
        if (UINT32_MAX - elapsed_ms < controller->config.poll_interval_ms) {
            elapsed_ms = controller->config.confirm_timeout_ms;
        }
        else {
            elapsed_ms += controller->config.poll_interval_ms;
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
                     &discovery_status, &read_error) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_MAIN, &main_present,
                     &main_status, &read_error) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_REMOTE, &remote_present,
                     &remote_status, &read_error) != ESP_OK) {
        snapshot->main.state = read_error;
        return result;
    }

    if (remote_present) {
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_CONFLICT, &remote_status);
        return result;
    }
    if (discovery_present && main_present) {
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_CONFLICT, &main_status);
        return result;
    }
    if (main_present) {
        if (!status_is_main(&main_status)) {
            node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_CONFLICT, &main_status);
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
        node_from_status(&snapshot->main, NRF_TOPOLOGY_NODE_CONFLICT, &discovery_status);
        return result;
    }

    nrf_upstream_status_t confirmed;
    const assign_confirm_result_t assign_result = assign_and_confirm(
        controller,
        io,
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

static bool verify_known_main(
    const nrf_topology_io_t *io,
    const nrf_topology_node_snapshot_t *known_main)
{
    nrf_upstream_status_t status;
    esp_err_t err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_MAIN, &status);
    if (err != ESP_OK || !status_is_main(&status)) {
        return false;
    }
    return status.hardware_id_length == known_main->hardware_id_length &&
           memcmp(status.hardware_id, known_main->hardware_id, status.hardware_id_length) == 0;
}

static bool close_mux_or_fault(const nrf_topology_io_t *io, nrf_topology_snapshot_t *snapshot)
{
    if (!io->tca_available) {
        return true;
    }
    if (io->mux_close(io->context) != ESP_OK) {
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

    if (io->mux_select(io->context, channel) != ESP_OK) {
        node->state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        if (!close_mux_or_fault(io, snapshot)) {
            *continue_scan = false;
        }
        return;
    }

    if (main_assigned && !verify_known_main(io, &snapshot->main)) {
        node->state = NRF_TOPOLOGY_NODE_CONFLICT;
        *main_lost = true;
        (void)close_mux_or_fault(io, snapshot);
        *continue_scan = false;
        return;
    }

    if (probe_status(io, NRF_UPSTREAM_ADDRESS_DISCOVERY, &discovery_present,
                     &discovery_status, &read_error) != ESP_OK) {
        node->state = read_error;
        if (!close_mux_or_fault(io, snapshot)) {
            *continue_scan = false;
        }
        return;
    }
    if (probe_status(io, NRF_UPSTREAM_ADDRESS_REMOTE, &remote_present,
                     &remote_status, &read_error) != ESP_OK) {
        node->state = read_error;
        if (!close_mux_or_fault(io, snapshot)) {
            *continue_scan = false;
        }
        return;
    }

    if (discovery_present && remote_present) {
        node_from_status(node, NRF_TOPOLOGY_NODE_CONFLICT, &remote_status);
    }
    else if (remote_present) {
        if (status_is_remote(&remote_status, channel)) {
            node_from_status(node, NRF_TOPOLOGY_NODE_ASSIGNED, &remote_status);
        }
        else {
            node_from_status(node, NRF_TOPOLOGY_NODE_CONFLICT, &remote_status);
        }
    }
    else if (discovery_present) {
        if (!status_is_unassigned_discovery(&discovery_status)) {
            node_from_status(node, NRF_TOPOLOGY_NODE_CONFLICT, &discovery_status);
        }
        else {
            nrf_upstream_status_t confirmed;
            const assign_confirm_result_t assign_result = assign_and_confirm(
                controller,
                io,
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

    if (!close_mux_or_fault(io, snapshot)) {
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
            }
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
        snapshot->main.state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        }
        return ESP_OK;
    }

    if (io->tca_available && !close_mux_or_fault(io, snapshot)) {
        snapshot->main.state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            snapshot->remotes[channel].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
        }
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
    detect_duplicate_hardware_ids(snapshot);
    return ESP_OK;
}
