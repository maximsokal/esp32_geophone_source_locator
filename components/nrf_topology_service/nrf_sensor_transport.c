#include "nrf_sensor_transport.h"

#include <stddef.h>
#include <string.h>

static bool hardware_id_equal(
    const nrf_topology_node_snapshot_t *left,
    const nrf_topology_node_snapshot_t *right)
{
    return left->hardware_id_length == right->hardware_id_length &&
           left->hardware_id_length > 0U &&
           memcmp(left->hardware_id, right->hardware_id, left->hardware_id_length) == 0;
}

static bool node_identity_equal(
    const nrf_topology_node_snapshot_t *left,
    const nrf_topology_node_snapshot_t *right)
{
    return hardware_id_equal(left, right) &&
           left->node_id == right->node_id &&
           left->mux_channel == right->mux_channel &&
           left->current_address == right->current_address &&
           left->assignment_request_id == right->assignment_request_id;
}

static nrf_sensor_cache_state_t state_for_sensor_result(uint8_t result)
{
    switch (result) {
    case NRF_UPSTREAM_SENSOR_RESULT_OK:
        return NRF_SENSOR_CACHE_OK;
    case NRF_UPSTREAM_SENSOR_RESULT_PARTIAL:
        return NRF_SENSOR_CACHE_PARTIAL;
    case NRF_UPSTREAM_SENSOR_RESULT_NO_DATA:
        return NRF_SENSOR_CACHE_NO_DATA;
    case NRF_UPSTREAM_SENSOR_RESULT_BUSY:
        return NRF_SENSOR_CACHE_SENSOR_BUSY;
    case NRF_UPSTREAM_SENSOR_RESULT_NOT_READY:
    case NRF_UPSTREAM_SENSOR_RESULT_NOT_REMOTE:
    case NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR:
    default:
        return NRF_SENSOR_CACHE_SENSOR_ERROR;
    }
}

static void cache_bind_identity(
    nrf_remote_sensor_cache_t *cache,
    const nrf_topology_node_snapshot_t *node,
    uint32_t generation)
{
    memset(cache, 0, sizeof(*cache));
    cache->identity_valid = true;
    cache->topology_current = true;
    cache->identity = *node;
    cache->topology_generation = generation;
    cache->state = NRF_SENSOR_CACHE_EMPTY;
    cache->last_attempt_state = NRF_SENSOR_CACHE_EMPTY;
}

static void cache_mark_topology_stale(nrf_remote_sensor_cache_t *cache)
{
    if (!cache->identity_valid) {
        return;
    }
    cache->topology_current = false;
    if (cache->state != NRF_SENSOR_CACHE_MUX_FAULT &&
        cache->state != NRF_SENSOR_CACHE_IDENTITY_STALE) {
        cache->state = NRF_SENSOR_CACHE_TOPOLOGY_STALE;
    }
}

esp_err_t nrf_sensor_transport_controller_init(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_config_t *config)
{
    if (controller == NULL || config == NULL ||
        config->timeout_ms == 0U || config->poll_interval_ms == 0U ||
        config->poll_interval_ms > config->timeout_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(controller, 0, sizeof(*controller));
    controller->config = *config;
    controller->next_request_id = config->request_seed == 0U ? 1U : config->request_seed;
    return ESP_OK;
}

uint8_t nrf_sensor_cache_reconcile_topology(
    const nrf_topology_snapshot_t *topology,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT])
{
    uint8_t initial_mask = 0U;
    size_t channel;

    if (topology == NULL || cache == NULL) {
        return 0U;
    }

    for (channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        const nrf_topology_node_snapshot_t *node = &topology->remotes[channel];
        nrf_remote_sensor_cache_t *entry = &cache[channel];

        if (node->state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            if (!entry->identity_valid || !node_identity_equal(&entry->identity, node)) {
                cache_bind_identity(entry, node, topology->generation);
            }
            else {
                entry->identity = *node;
                entry->topology_generation = topology->generation;
                entry->topology_current = true;
                if (entry->state == NRF_SENSOR_CACHE_TOPOLOGY_STALE) {
                    entry->state = entry->last_attempt_state;
                }
            }

            if (!entry->initial_refresh_issued) {
                entry->initial_refresh_issued = true;
                initial_mask |= (uint8_t)(1U << channel);
            }
            continue;
        }

        if (node->state == NRF_TOPOLOGY_NODE_ABSENT) {
            memset(entry, 0, sizeof(*entry));
            entry->state = NRF_SENSOR_CACHE_EMPTY;
            entry->last_attempt_state = NRF_SENSOR_CACHE_EMPTY;
            continue;
        }

        cache_mark_topology_stale(entry);
    }

    return initial_mask;
}

static uint32_t next_request_id(nrf_sensor_transport_controller_t *controller)
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

static bool status_matches_node(
    const nrf_upstream_status_t *status,
    const nrf_topology_node_snapshot_t *node,
    uint8_t expected_role)
{
    if (status == NULL || node == NULL ||
        status->hardware_id_length != node->hardware_id_length ||
        status->hardware_id_length == 0U ||
        memcmp(status->hardware_id, node->hardware_id, status->hardware_id_length) != 0 ||
        status->request_id != node->assignment_request_id ||
        status->role != expected_role ||
        status->node_id != node->node_id ||
        status->mux_channel != node->mux_channel ||
        status->target_address != node->current_address ||
        status->lifecycle != NRF_UPSTREAM_LIFECYCLE_READY ||
        status->result != NRF_UPSTREAM_RESULT_OK ||
        (status->flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) == 0U ||
        (status->flags & NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED) == 0U) {
        return false;
    }
    return true;
}

static void mark_failure(
    nrf_remote_sensor_cache_t *cache,
    nrf_sensor_cache_state_t state,
    bool stale)
{
    cache->state = state;
    cache->last_attempt_state = state;
    if (stale) {
        cache->topology_current = false;
    }
}

static bool io_aborted(const nrf_sensor_transport_io_t *io)
{
    return io->abort_requested != NULL && io->abort_requested(io->context);
}

static esp_err_t close_with_dominance(
    const nrf_sensor_transport_io_t *io,
    nrf_remote_sensor_cache_t *cache,
    bool *request_topology_reconcile)
{
    const esp_err_t close_err = io->mux_close(io->context);
    if (close_err != ESP_OK) {
        const bool faulted = io->mux_faulted != NULL && io->mux_faulted(io->context);
        cache->state = faulted ? NRF_SENSOR_CACHE_MUX_FAULT : NRF_SENSOR_CACHE_TRANSPORT_ERROR;
        cache->last_attempt_state = cache->state;
        cache->topology_current = false;
        *request_topology_reconcile = true;
    }
    return close_err;
}

static void mark_read_failure(
    esp_err_t err,
    nrf_remote_sensor_cache_t *cache,
    bool *request_topology_reconcile)
{
    if (err == ESP_ERR_INVALID_RESPONSE) {
        mark_failure(cache, NRF_SENSOR_CACHE_PROTOCOL_ERROR, true);
    }
    else if (err == ESP_ERR_NOT_FOUND) {
        mark_failure(cache, NRF_SENSOR_CACHE_IDENTITY_STALE, true);
    }
    else {
        mark_failure(cache, NRF_SENSOR_CACHE_TRANSPORT_ERROR, true);
    }
    *request_topology_reconcile = true;
}

static bool deadline_reached(
    const nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    uint64_t started_ms)
{
    const uint64_t now = io->now_ms(io->context);
    return now - started_ms >= (uint64_t)controller->config.timeout_ms;
}

esp_err_t nrf_sensor_transport_refresh_channel(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    const nrf_topology_snapshot_t *topology,
    uint8_t channel,
    nrf_remote_sensor_cache_t *cache,
    bool *request_topology_reconcile)
{
    nrf_upstream_status_t status;
    nrf_upstream_sensor_snapshot_t sensor;
    nrf_upstream_sensor_refresh_t request;
    const nrf_topology_node_snapshot_t *remote;
    uint64_t started_ms;
    bool resend_used = false;
    bool selected = false;
    esp_err_t err = ESP_OK;

    if (controller == NULL || io == NULL || topology == NULL || cache == NULL ||
        request_topology_reconcile == NULL || channel >= NRF_TOPOLOGY_REMOTE_COUNT ||
        io->mux_select == NULL || io->mux_close == NULL || io->read_status == NULL ||
        io->write_sensor_refresh == NULL || io->read_remote_snapshot == NULL ||
        io->sleep_ms == NULL || io->now_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *request_topology_reconcile = false;
    remote = &topology->remotes[channel];
    if (!topology->tca_available || topology->tca_fault) {
        mark_failure(
            cache,
            topology->tca_fault ? NRF_SENSOR_CACHE_MUX_FAULT : NRF_SENSOR_CACHE_TOPOLOGY_STALE,
            true);
        *request_topology_reconcile = topology->tca_fault;
        return ESP_ERR_INVALID_STATE;
    }
    if (remote->state != NRF_TOPOLOGY_NODE_ASSIGNED || !cache->identity_valid ||
        !node_identity_equal(&cache->identity, remote)) {
        mark_failure(cache, NRF_SENSOR_CACHE_TOPOLOGY_STALE, true);
        *request_topology_reconcile = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED &&
        topology->main.state != NRF_TOPOLOGY_NODE_ABSENT) {
        mark_failure(cache, NRF_SENSOR_CACHE_TOPOLOGY_STALE, true);
        *request_topology_reconcile = true;
        return ESP_ERR_INVALID_STATE;
    }

    request.request_id = next_request_id(controller);
    cache->sensor_request_id = request.request_id;
    cache->state = NRF_SENSOR_CACHE_PENDING;
    cache->last_attempt_state = NRF_SENSOR_CACHE_PENDING;
    started_ms = io->now_ms(io->context);

    if (io_aborted(io)) {
        mark_failure(cache, NRF_SENSOR_CACHE_ABORTED, false);
        return ESP_ERR_INVALID_STATE;
    }

    err = io->mux_select(io->context, channel);
    if (err != ESP_OK) {
        mark_failure(cache, NRF_SENSOR_CACHE_TRANSPORT_ERROR, true);
        *request_topology_reconcile = true;
        {
            const esp_err_t close_err = close_with_dominance(io, cache, request_topology_reconcile);
            return close_err != ESP_OK ? close_err : err;
        }
    }
    selected = true;

    if (topology->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        memset(&status, 0, sizeof(status));
        err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_MAIN, &status);
        if (err != ESP_OK) {
            mark_read_failure(err, cache, request_topology_reconcile);
            goto done;
        }
        if (!status_matches_node(&status, &topology->main, NRF_UPSTREAM_ROLE_MAIN)) {
            mark_failure(cache, NRF_SENSOR_CACHE_IDENTITY_STALE, true);
            *request_topology_reconcile = true;
            err = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
    }

    memset(&status, 0, sizeof(status));
    err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_REMOTE, &status);
    if (err != ESP_OK) {
        mark_read_failure(err, cache, request_topology_reconcile);
        goto done;
    }
    if (!status_matches_node(&status, remote, NRF_UPSTREAM_ROLE_REMOTE)) {
        mark_failure(cache, NRF_SENSOR_CACHE_IDENTITY_STALE, true);
        *request_topology_reconcile = true;
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    err = io->write_sensor_refresh(io->context, &request);
    if (err != ESP_OK) {
        mark_read_failure(err, cache, request_topology_reconcile);
        goto done;
    }

    for (;;) {
        if (io_aborted(io)) {
            mark_failure(cache, NRF_SENSOR_CACHE_ABORTED, false);
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }
        if (deadline_reached(controller, io, started_ms)) {
            mark_failure(cache, NRF_SENSOR_CACHE_TIMEOUT, false);
            err = ESP_ERR_TIMEOUT;
            goto done;
        }

        memset(&status, 0, sizeof(status));
        memset(&sensor, 0, sizeof(sensor));
        err = io->read_remote_snapshot(io->context, &status, &sensor);
        if (err != ESP_OK) {
            mark_read_failure(err, cache, request_topology_reconcile);
            goto done;
        }
        if (!status_matches_node(&status, remote, NRF_UPSTREAM_ROLE_REMOTE)) {
            mark_failure(cache, NRF_SENSOR_CACHE_IDENTITY_STALE, true);
            *request_topology_reconcile = true;
            err = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }

        if (sensor.request_id == request.request_id) {
            if (sensor.result == NRF_UPSTREAM_SENSOR_RESULT_PENDING) {
                /* Keep polling the accepted request. */
            }
            else {
                cache->sensor = sensor;
                cache->response_valid = true;
                cache->esp_receipt_uptime_ms = io->now_ms(io->context);
                cache->state = state_for_sensor_result(sensor.result);
                cache->last_attempt_state = cache->state;
                if (sensor.result == NRF_UPSTREAM_SENSOR_RESULT_NOT_REMOTE) {
                    cache->topology_current = false;
                    *request_topology_reconcile = true;
                }
                err = ESP_OK;
                goto done;
            }
        }
        else if (sensor.result != NRF_UPSTREAM_SENSOR_RESULT_PENDING) {
            if (resend_used) {
                mark_failure(cache, NRF_SENSOR_CACHE_SENSOR_BUSY, false);
                err = ESP_ERR_INVALID_STATE;
                goto done;
            }
            err = io->write_sensor_refresh(io->context, &request);
            if (err != ESP_OK) {
                mark_read_failure(err, cache, request_topology_reconcile);
                goto done;
            }
            resend_used = true;
        }

        io->sleep_ms(io->context, controller->config.poll_interval_ms);
    }

done:
    if (selected) {
        const esp_err_t close_err = close_with_dominance(io, cache, request_topology_reconcile);
        if (close_err != ESP_OK) {
            return close_err;
        }
    }
    return err;
}

esp_err_t nrf_sensor_transport_capture_height_reference(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    const nrf_topology_snapshot_t *topology,
    uint8_t channel,
    uint16_t generation,
    bool *request_topology_reconcile)
{
    nrf_upstream_status_t status;
    nrf_upstream_height_reference_t request;
    const nrf_topology_node_snapshot_t *remote;
    bool selected = false;
    esp_err_t err;

    if (controller == NULL || io == NULL || topology == NULL ||
        request_topology_reconcile == NULL || channel >= NRF_TOPOLOGY_REMOTE_COUNT ||
        generation == 0U || io->mux_select == NULL || io->mux_close == NULL ||
        io->read_status == NULL || io->write_height_reference == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *request_topology_reconcile = false;
    remote = &topology->remotes[channel];
    if (!topology->tca_available || topology->tca_fault ||
        remote->state != NRF_TOPOLOGY_NODE_ASSIGNED ||
        (topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED &&
         topology->main.state != NRF_TOPOLOGY_NODE_ABSENT)) {
        *request_topology_reconcile = topology->tca_fault ||
            remote->state == NRF_TOPOLOGY_NODE_ASSIGNED;
        return ESP_ERR_INVALID_STATE;
    }
    if (io->abort_requested != NULL && io->abort_requested(io->context)) {
        return ESP_ERR_INVALID_STATE;
    }

    err = io->mux_select(io->context, channel);
    if (err != ESP_OK) {
        *request_topology_reconcile = true;
        const esp_err_t close_err = io->mux_close(io->context);
        return close_err != ESP_OK ? close_err : err;
    }
    selected = true;

    if (topology->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        memset(&status, 0, sizeof(status));
        err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_MAIN, &status);
        if (err != ESP_OK || !status_matches_node(&status, &topology->main, NRF_UPSTREAM_ROLE_MAIN)) {
            *request_topology_reconcile = true;
            err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
            goto done;
        }
    }

    memset(&status, 0, sizeof(status));
    err = io->read_status(io->context, NRF_UPSTREAM_ADDRESS_REMOTE, &status);
    if (err != ESP_OK || !status_matches_node(&status, remote, NRF_UPSTREAM_ROLE_REMOTE)) {
        *request_topology_reconcile = true;
        err = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        goto done;
    }

    request.action = NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE;
    request.request_id = next_request_id(controller);
    request.generation = generation;
    err = io->write_height_reference(io->context, &request);
    if (err != ESP_OK) {
        *request_topology_reconcile = true;
    }

done:
    if (selected) {
        const esp_err_t close_err = io->mux_close(io->context);
        if (close_err != ESP_OK) {
            *request_topology_reconcile = true;
            return close_err;
        }
    }
    return err;
}
