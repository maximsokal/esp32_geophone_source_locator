#include "nrf_log_collector.h"
#include "runtime_observability.h"
#include "sensor_bus_errors.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nrf_log_protocol.h"
#include "nrf_topology_periods.h"
#include "nrf_upstream_protocol.h"

#define NRF_LOG_REQUEST_RETRIES 1U
#define NRF_LOG_STAGED_RECORDS (NRF_LOG_PROTOCOL_MAX_RECORDS * 2U + 1U)
#ifndef CONFIG_NRF_LOG_POLL_BACKOFF_MAX_ROUNDS
#define CONFIG_NRF_LOG_POLL_BACKOFF_MAX_ROUNDS 32U
#endif

_Static_assert(sizeof(nrf_log_stored_record_t) == 28U, "nRF stored log record must stay 28 bytes");
_Static_assert(
    NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
        NRF_LOG_PROTOCOL_RECORD_LENGTH * NRF_LOG_PROTOCOL_MAX_RECORDS ==
        NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH,
    "a block carries its base header plus fixed-size records");

typedef struct {
    bool present;
    bool link_ready;
    bool identity_valid;
    bool logs_unsupported;
    bool boot_id_valid;
    bool cursor_valid;
    bool firmware_reported;
    bool pending_status_valid;
    bool pending_gap_counted;
    uint8_t pending_blocks;
    nrf_log_status_t pending_status;
    uint64_t pending_status_ms;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t node_id;
    uint8_t mux_channel;
    uint8_t current_address;
    uint32_t assignment_generation;
    uint32_t next_request_id;
    uint32_t boot_id;
    uint32_t cursor_seq;
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
    uint32_t wire_gap_total;
    uint32_t local_drop_total;
    uint8_t backoff_rounds;
    uint8_t backoff_remaining;
    uint16_t count;
    uint8_t head;
} nrf_log_node_state_t;

struct nrf_log_collector {
    SemaphoreHandle_t mutex;
    uint8_t next_node_index;
    uint8_t poll_next_start_index;
    bool poll_in_progress;
    uint16_t poll_visited_mask;
    bool has_polled;
    uint64_t last_poll_ms;
    nrf_log_node_state_t nodes[NRF_LOG_COLLECTOR_NODE_COUNT];
    nrf_log_stored_record_t records[NRF_LOG_COLLECTOR_NODE_COUNT][NRF_LOG_COLLECTOR_RECORD_CAPACITY];
};

_Static_assert(
    sizeof(((nrf_log_collector_t *)0)->records) ==
        NRF_LOG_COLLECTOR_NODE_COUNT * NRF_LOG_COLLECTOR_RECORD_CAPACITY * 28U,
    "nRF log record store size changed");

typedef struct {
    nrf_log_stored_record_t records[NRF_LOG_STAGED_RECORDS];
    uint8_t count;
    bool boot_changed;
    uint32_t prior_boot_id;
    uint32_t boot_id;
    uint32_t cursor_seq;
    bool cursor_valid;
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
    uint32_t wire_gap_increments;
} nrf_log_poll_stage_t;

static bool collector_lock(nrf_log_collector_t *collector)
{
    return xSemaphoreTake(collector->mutex, portMAX_DELAY) == pdTRUE;
}

static void collector_unlock(nrf_log_collector_t *collector)
{
    (void)xSemaphoreGive(collector->mutex);
}

static uint32_t saturating_add(uint32_t value, uint32_t amount)
{
    return UINT32_MAX - value < amount ? UINT32_MAX : value + amount;
}

static uint32_t next_nonzero(uint32_t value)
{
    ++value;
    return value == 0U ? 1U : value;
}

static uint32_t next_status_request_id(uint8_t node_index, uint32_t previous)
{
    /* Four IDs per node; a 64-ID stride covers MAIN plus all eight mux channels. */
    const uint32_t first = (uint32_t)node_index * 4U + 1U;
    return previous == 0U ? first : previous + 64U;
}

static uint32_t elapsed_ms(const nrf_log_collector_io_t *io, uint64_t started_ms)
{
    const uint64_t now = io->now_ms(io->context);
    const uint64_t elapsed = now >= started_ms ? now - started_ms : 0U;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static bool should_yield(const nrf_log_collector_io_t *io)
{
    return io->yield_requested != NULL && io->yield_requested(io->context);
}

static void report_link_failure(const nrf_log_collector_io_t *io, uint8_t node_index)
{
    if (io->report_link_failure != NULL) {
        io->report_link_failure(io->context, node_index);
    }
}

static bool should_stop_link_retry(const nrf_log_collector_io_t *io, uint8_t node_index)
{
    return io->node_link_ready != NULL &&
        !io->node_link_ready(io->context, node_index);
}

static bool budget_limit_reached(
    const nrf_log_collector_io_t *io,
    uint64_t started_ms,
    uint32_t budget_ms)
{
    const uint32_t elapsed = elapsed_ms(io, started_ms);
    return elapsed >= budget_ms || budget_ms - elapsed <= io->mux_close_reserve_ms;
}

static esp_err_t exchange_frame(
    const nrf_log_collector_io_t *io,
    uint8_t node_index,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity,
    size_t *received_length,
    uint64_t deadline_ms,
    bool *attempted,
    bool *yielded,
    bool *completed)
{
    const uint64_t now_ms = io->now_ms(io->context);
    const uint64_t reserve_ms = (uint64_t)io->exchange_timeout_ms +
        io->mux_close_reserve_ms + io->scheduler_reserve_ms;
    bool node_open = false;
    esp_err_t err;

    *completed = false;
    *received_length = 0U;
    if (should_yield(io) || now_ms >= deadline_ms || deadline_ms - now_ms < reserve_ms + io->session_open_timeout_ms) {
        *yielded = true;
        return ESP_OK;
    }

    io->set_deadline(io->context, deadline_ms - reserve_ms);
    err = io->begin_node(io->context, node_index);
    if (err == SENSOR_BUS_ERR_MUTEX_BUSY) {
        *yielded = true;
        return ESP_OK;
    }
    node_open = true;
    *attempted = true;
    if (err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
        *yielded = true;
        err = ESP_OK;
    }
    const uint64_t after_open_ms = io->now_ms(io->context);
    if (err == ESP_OK && (should_yield(io) || after_open_ms >= deadline_ms ||
            deadline_ms - after_open_ms < reserve_ms)) {
        *yielded = true;
    }
    if (err == ESP_OK && !*yielded) {
        io->set_deadline(io->context, deadline_ms - io->mux_close_reserve_ms - io->scheduler_reserve_ms);
        err = io->write_frame(io->context, node_index, request, request_length);
        if (err == ESP_OK) {
            err = io->read_frame(
                io->context, node_index, response, response_capacity, received_length);
        }
        if (err == ESP_OK) {
            *completed = true;
        } else if (err == SENSOR_BUS_ERR_MUTEX_BUSY ||
                   err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
            *yielded = true;
            err = ESP_OK;
        }
    }

    if (node_open) {
        io->set_deadline(io->context, deadline_ms - io->scheduler_reserve_ms);
        const esp_err_t close_err = io->end_node(io->context, node_index);
        if (close_err == SENSOR_BUS_ERR_OWNER_HOLD_BUDGET) {
            *yielded = true;
        } else if (err == ESP_OK && close_err != ESP_OK) {
            err = close_err;
        }
    }
    return err;
}

static bool identity_equal(const nrf_log_node_state_t *state, const nrf_log_poll_target_t *target)
{
    return state->identity_valid &&
           state->assignment_generation == target->assignment_generation &&
           state->hardware_id_length == target->hardware_id_length &&
           state->node_id == target->node_id &&
           state->mux_channel == target->mux_channel &&
           state->current_address == target->current_address &&
           memcmp(state->hardware_id, target->hardware_id, target->hardware_id_length) == 0;
}

static bool same_assignment(const nrf_log_node_state_t *a, const nrf_log_node_state_t *b)
{
    return a->identity_valid == b->identity_valid &&
           a->assignment_generation == b->assignment_generation &&
           a->hardware_id_length == b->hardware_id_length &&
           a->node_id == b->node_id &&
           a->mux_channel == b->mux_channel &&
           a->current_address == b->current_address &&
           memcmp(a->hardware_id, b->hardware_id, a->hardware_id_length) == 0;
}

static void state_set_identity(nrf_log_node_state_t *state, const nrf_log_poll_target_t *target)
{
    state->identity_valid = true;
    state->assignment_generation = target->assignment_generation;
    state->hardware_id_length = target->hardware_id_length;
    memcpy(state->hardware_id, target->hardware_id, target->hardware_id_length);
    state->node_id = target->node_id;
    state->mux_channel = target->mux_channel;
    state->current_address = target->current_address;
}

static void append_record_locked(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    const nrf_log_stored_record_t *record)
{
    nrf_log_node_state_t *state = &collector->nodes[node_index];
    collector->records[node_index][state->head] = *record;
    state->head = (uint8_t)((state->head + 1U) % NRF_LOG_COLLECTOR_RECORD_CAPACITY);
    if (state->count < NRF_LOG_COLLECTOR_RECORD_CAPACITY) {
        ++state->count;
    } else {
        state->local_drop_total = saturating_add(state->local_drop_total, 1U);
    }
}

static void stage_reboot_marker(
    nrf_log_poll_stage_t *stage,
    uint32_t old_boot_id,
    uint32_t new_boot_id,
    uint32_t esp_rx_uptime_ms)
{
    nrf_log_stored_record_t *record;
    if (stage->count >= NRF_LOG_STAGED_RECORDS) return;
    record = &stage->records[stage->count++];
    memset(record, 0, sizeof(*record));
    record->boot_id = new_boot_id;
    /* T0 rolls over before this reserved sequence value; WebUI tokens stay opaque. */
    record->seq = UINT32_MAX;
    record->esp_rx_uptime_ms = esp_rx_uptime_ms;
    record->event_id = NRF_LOG_COLLECTOR_REBOOT_EVENT_ID;
    record->severity = NRF_LOG_SEVERITY_WARN;
    record->subsystem = NRF_LOG_SUBSYSTEM_BOOT;
    record->arg0 = old_boot_id;
    record->arg1 = new_boot_id;
}

static bool legacy_status_reply(const uint8_t *frame, size_t received_length)
{
    nrf_upstream_status_t status;
    size_t frame_length;
    if (frame == NULL || received_length < 6U) return false;
    frame_length = (size_t)frame[4] | ((size_t)frame[5] << 8U);
    return frame_length <= received_length && frame_length >= 6U &&
           nrf_upstream_protocol_decode_status(frame, frame_length, &status) == NRF_UPSTREAM_RESULT_OK;
}

esp_err_t nrf_log_collector_read_status_with_probe(
    const nrf_log_collector_io_t *io,
    uint8_t node_index,
    uint8_t *next_probe,
    bool probe_allowed,
    uint8_t *frame,
    size_t *received_length)
{
    static const uint8_t lengths[] = {28U, 31U, 32U, 33U, NRF_LOG_PROTOCOL_STATUS_LENGTH};
    uint8_t prefix[33U];
    size_t prefix_length = 0U;
    esp_err_t err;
    if (received_length != NULL) *received_length = 0U;
    if (io == NULL || io->read_frame == NULL || next_probe == NULL || frame == NULL ||
        received_length == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t step = (uint8_t)(*next_probe % sizeof(lengths));
    const size_t length = lengths[step];
    if (probe_allowed || length == NRF_LOG_PROTOCOL_STATUS_LENGTH) {
        *next_probe = (uint8_t)((step + 1U) % sizeof(lengths));
        if (length < NRF_LOG_PROTOCOL_STATUS_LENGTH) {
            err = io->read_frame(io->context, node_index, prefix, length, &prefix_length);
            if (err != ESP_OK) return err;
            if (prefix_length != length) return ESP_ERR_INVALID_RESPONSE;
        }
    }
    err = io->read_frame(
        io->context, node_index, frame, NRF_LOG_PROTOCOL_STATUS_LENGTH, received_length);
    if (err != ESP_OK || *received_length != NRF_LOG_PROTOCOL_STATUS_LENGTH ||
        (prefix_length != 0U && memcmp(prefix, frame, prefix_length) != 0)) {
        *received_length = 0U;
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t exchange_status(
    const nrf_log_collector_io_t *io,
    uint8_t node_index,
    const nrf_log_status_request_t *request,
    uint64_t deadline_ms,
    nrf_log_status_t *status,
    bool *attempted,
    bool *yielded,
    bool *exchange_completed)
{
    uint8_t request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint8_t response_frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    const nrf_log_error_t encoded = nrf_log_protocol_encode_status_request(request, request_frame);
    if (encoded != NRF_LOG_ERROR_OK) return ESP_ERR_INVALID_ARG;
    *exchange_completed = false;

    esp_err_t last_error = ESP_OK;
    for (uint8_t attempt = 0U; attempt <= NRF_LOG_REQUEST_RETRIES; ++attempt) {
        size_t received_length = 0U;
        bool pair_completed = false;
        esp_err_t err;
        err = exchange_frame(
            io,
            node_index,
            request_frame,
            sizeof(request_frame),
            response_frame,
            sizeof(response_frame),
            &received_length,
            deadline_ms,
            attempted,
            yielded,
            &pair_completed);
        if (!pair_completed && err == ESP_OK) return last_error;
        if (err == ESP_OK && legacy_status_reply(response_frame, received_length)) {
            report_link_failure(io, node_index);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (err == ESP_OK && received_length < sizeof(response_frame)) err = ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) {
            err = nrf_log_protocol_decode_status(
                response_frame, sizeof(response_frame), status) == NRF_LOG_ERROR_OK
                    ? ESP_OK
                    : ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK && status->request_id != request->request_id) err = ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) {
            *exchange_completed = true;
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_RESPONSE) {
            report_link_failure(io, node_index);
        }
        last_error = err;
        if (should_stop_link_retry(io, node_index)) return err;
        if (err == ESP_ERR_NOT_SUPPORTED || attempt == NRF_LOG_REQUEST_RETRIES) return err;
        if (should_yield(io)) return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/*
 * Records the nRF can return for this cursor: a block never carries more than
 * the request allows, so asking for what is pending bounds both the reply and
 * the time the bus is held (136 bytes need 14.3 ms at 100 kHz, one record 5 ms).
 */
static uint8_t block_request_records(const nrf_log_status_t *status, uint32_t cursor_seq)
{
    const uint32_t start = cursor_seq < status->oldest_seq ? status->oldest_seq : cursor_seq;
    const uint32_t pending = status->next_seq > start ? status->next_seq - start : 0U;

    if (pending == 0U) return 1U;
    return pending > NRF_LOG_PROTOCOL_MAX_RECORDS
        ? (uint8_t)NRF_LOG_PROTOCOL_MAX_RECORDS : (uint8_t)pending;
}

static esp_err_t exchange_block(
    const nrf_log_collector_io_t *io,
    uint8_t node_index,
    const nrf_log_read_request_t *request,
    const nrf_log_status_t *status,
    uint32_t cursor_seq,
    uint64_t deadline_ms,
    nrf_log_block_t *block,
    bool *attempted,
    bool *yielded,
    bool *exchange_completed)
{
    uint8_t request_frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    uint8_t response_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    const nrf_log_error_t encoded = nrf_log_protocol_encode_read_request(request, request_frame);
    if (encoded != NRF_LOG_ERROR_OK) return ESP_ERR_INVALID_ARG;
    const size_t response_capacity = NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
        (size_t)NRF_LOG_PROTOCOL_RECORD_LENGTH * request->max_records;
    *exchange_completed = false;

    esp_err_t last_error = ESP_OK;
    for (uint8_t attempt = 0U; attempt <= NRF_LOG_REQUEST_RETRIES; ++attempt) {
        size_t received_length = 0U;
        size_t response_length;
        bool pair_completed = false;
        esp_err_t err;
        nrf_log_block_validation_t validation;
        err = exchange_frame(
            io,
            node_index,
            request_frame,
            sizeof(request_frame),
            response_frame,
            response_capacity,
            &received_length,
            deadline_ms,
            attempted,
            yielded,
            &pair_completed);
        if (!pair_completed && err == ESP_OK) return last_error;
        if (err == ESP_OK && received_length < NRF_LOG_PROTOCOL_HEADER_LENGTH) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        response_length = err == ESP_OK
            ? (size_t)response_frame[4] | ((size_t)response_frame[5] << 8U)
            : 0U;
        if (err == ESP_OK &&
            (response_length > received_length || response_length > sizeof(response_frame))) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK) {
            memset(&validation, 0, sizeof(validation));
            validation.has_expected_boot_id = true;
            validation.expected_boot_id = status->boot_id;
            validation.has_max_records = true;
            validation.max_records = request->max_records;
            if (nrf_log_protocol_decode_block(
                    response_frame, response_length, &validation, block) != NRF_LOG_ERROR_OK) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (err == ESP_OK && block->request_id != request->request_id) err = ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK && block->boot_id == status->boot_id) {
            memset(&validation, 0, sizeof(validation));
            validation.has_expected_boot_id = true;
            validation.expected_boot_id = status->boot_id;
            validation.has_cursor = true;
            validation.cursor_seq = cursor_seq;
            validation.has_max_records = true;
            validation.max_records = request->max_records;
            validation.has_status_next_seq = true;
            validation.status_next_seq = status->next_seq;
            if (nrf_log_protocol_decode_block(
                    response_frame, response_length, &validation, block) != NRF_LOG_ERROR_OK) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        } else if (err == ESP_OK) {
            /* A reboot can occur after STATUS. The T0 mismatch block is empty by contract. */
            memset(&validation, 0, sizeof(validation));
            validation.has_expected_boot_id = true;
            validation.expected_boot_id = status->boot_id;
            validation.has_max_records = true;
            validation.max_records = request->max_records;
            if (nrf_log_protocol_decode_block(
                    response_frame, response_length, &validation, block) != NRF_LOG_ERROR_OK) {
                err = ESP_ERR_INVALID_RESPONSE;
            }
        }
        if (err == ESP_OK) {
            *exchange_completed = true;
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_RESPONSE) {
            report_link_failure(io, node_index);
        }
        last_error = err;
        if (should_stop_link_retry(io, node_index)) return err;
        if (attempt == NRF_LOG_REQUEST_RETRIES) return err;
        if (should_yield(io)) return err;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t exchange_firmware(
    const nrf_log_collector_io_t *io,
    uint8_t node_index,
    uint32_t request_id,
    uint32_t boot_id,
    nrf_log_firmware_info_t *info,
    uint64_t deadline_ms,
    bool *attempted,
    bool *yielded,
    bool *exchange_completed)
{
    const nrf_log_status_request_t request = {.request_id = request_id};
    uint8_t request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint8_t response_frame[NRF_LOG_PROTOCOL_FIRMWARE_LENGTH];
    size_t received = 0U;
    bool pair_completed = false;
    if (nrf_log_protocol_encode_firmware_request(&request, request_frame) != NRF_LOG_ERROR_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    *exchange_completed = false;
    esp_err_t err = exchange_frame(
        io,
        node_index,
        request_frame,
        sizeof(request_frame),
        response_frame,
        sizeof(response_frame),
        &received,
        deadline_ms,
        attempted,
        yielded,
        &pair_completed);
    if (err != ESP_OK || !pair_completed) return err;
    if (err == ESP_OK && (received != sizeof(response_frame) ||
        nrf_log_protocol_decode_firmware(response_frame, received, info) != NRF_LOG_ERROR_OK ||
        info->request_id != request_id || info->boot_id != boot_id)) {
        err = ESP_ERR_INVALID_RESPONSE;
        report_link_failure(io, node_index);
    }
    if (err == ESP_OK) *exchange_completed = true;
    return err;
}

static void stage_record(
    nrf_log_poll_stage_t *stage,
    uint32_t boot_id,
    const nrf_log_record_t *source,
    uint32_t esp_rx_uptime_ms)
{
    nrf_log_stored_record_t *record;
    if (stage->count >= NRF_LOG_STAGED_RECORDS) return;
    record = &stage->records[stage->count++];
    record->boot_id = boot_id;
    record->seq = source->seq;
    record->uptime_ms = source->uptime_ms;
    record->esp_rx_uptime_ms = esp_rx_uptime_ms;
    record->event_id = source->event_id;
    record->severity = source->severity;
    record->subsystem = source->subsystem;
    record->arg0 = source->arg0;
    record->arg1 = source->arg1;
}

static esp_err_t poll_node(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    const nrf_log_collector_io_t *io,
    uint64_t iteration_started_ms,
    uint32_t per_node_budget_ms,
    uint32_t total_budget_ms,
    bool *attempted,
    bool *yielded,
    bool *progressed)
{
    nrf_log_node_state_t old_state;
    nrf_log_poll_stage_t stage;
    nrf_log_status_request_t status_request = {0};
    nrf_log_status_t status = {0};
    esp_err_t err = ESP_OK;
    bool status_valid = false;
    bool stage_gap_counted = false;
    uint8_t blocks_completed = 0U;
    bool pending = false;
    const uint64_t node_started_ms = io->now_ms(io->context);
    const uint64_t node_deadline_ms = node_started_ms + per_node_budget_ms;
    const uint64_t iteration_deadline_ms = iteration_started_ms + total_budget_ms;
    const uint64_t deadline_ms = node_deadline_ms < iteration_deadline_ms
        ? node_deadline_ms : iteration_deadline_ms;
    bool firmware_needed = false;
    bool firmware_supported = false;
    bool firmware_exchange_completed = false;
    nrf_log_firmware_info_t firmware = {0};
    uint32_t status_request_id;

    *attempted = false;
    *yielded = false;
    *progressed = false;
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    old_state = collector->nodes[node_index];
    if (old_state.pending_status_valid && node_started_ms >= old_state.pending_status_ms &&
        node_started_ms - old_state.pending_status_ms >= NRF_LOG_COLLECTOR_PERIOD_MS) {
        /* A long urgent-work pause can outlive the remote ring snapshot. */
        old_state.pending_status_valid = false;
    }
    status_request_id = old_state.pending_status_valid ? old_state.pending_status.request_id
        : next_status_request_id(node_index, old_state.next_request_id);
    collector->nodes[node_index].next_request_id = status_request_id;
    collector_unlock(collector);

    memset(&stage, 0, sizeof(stage));
    status_request.request_id = status_request_id;
    if (old_state.pending_status_valid) {
        status = old_state.pending_status;
        status_valid = true;
        blocks_completed = old_state.pending_blocks;
        stage_gap_counted = old_state.pending_gap_counted;
    } else {
        err = exchange_status(io, node_index, &status_request, deadline_ms,
            &status, attempted, yielded, &status_valid);
        *progressed = status_valid;
        old_state.pending_status_ms = io->now_ms(io->context);
    }

    if (err == ESP_OK && status_valid) {
        const bool reboot = old_state.boot_id_valid && old_state.boot_id != status.boot_id;
        pending = true;
        stage.boot_changed = reboot;
        stage.prior_boot_id = old_state.boot_id;
        stage.boot_id = status.boot_id;
        stage.cursor_valid = true;
        stage.cursor_seq = old_state.boot_id_valid && old_state.boot_id == status.boot_id && old_state.cursor_valid
            ? old_state.cursor_seq
            : status.oldest_seq;
        stage.overwritten_total = status.overwritten_total;
        stage.lost_internal_total = status.lost_internal_total;
        if (!stage_gap_counted && old_state.boot_id_valid && old_state.boot_id == status.boot_id &&
            old_state.cursor_valid && old_state.cursor_seq < status.oldest_seq) {
            stage.wire_gap_increments = 1U;
            stage_gap_counted = true;
        }
        if (reboot) {
            stage_reboot_marker(&stage, old_state.boot_id, status.boot_id, (uint32_t)io->now_ms(io->context));
        }
        if (stage.cursor_seq > status.next_seq) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        firmware_needed = !old_state.firmware_reported ||
            !old_state.boot_id_valid || old_state.boot_id != status.boot_id;
        firmware_supported = (status.flags & NRF_LOG_STATUS_FLAG_FIRMWARE_INFO) != 0U;
        firmware.boot_id = status.boot_id;
        firmware_exchange_completed = firmware_needed && !firmware_supported;
        if (firmware_needed && firmware_supported && err == ESP_OK) {
            if (should_yield(io)) {
                *yielded = true;
            } else {
                err = exchange_firmware(io, node_index, status.request_id + 3U,
                    status.boot_id, &firmware, deadline_ms, attempted, yielded,
                    &firmware_exchange_completed);
                *progressed = *progressed || firmware_exchange_completed;
            }
        }
        for (uint8_t block_index = blocks_completed;
             err == ESP_OK && block_index < 2U && !*yielded;
             ++block_index) {
            nrf_log_read_request_t read_request;
            nrf_log_block_t block;
            bool block_exchange_completed = false;
            const bool needs_read = status.available != 0U &&
                (stage.cursor_seq < status.next_seq || stage.cursor_seq < status.oldest_seq);
            if (!needs_read) {
                pending = false;
                break;
            }
            read_request.request_id = next_nonzero(status_request.request_id);
            if (block_index != 0U) read_request.request_id = next_nonzero(read_request.request_id);
            read_request.expected_boot_id = stage.boot_id;
            read_request.cursor_seq = stage.cursor_seq;
            read_request.max_records = block_request_records(&status, stage.cursor_seq);
            err = exchange_block(
                io,
                node_index,
                &read_request,
                &status,
                stage.cursor_seq,
                deadline_ms,
                &block,
                attempted,
                yielded,
                &block_exchange_completed);
            if (err != ESP_OK || !block_exchange_completed) break;
            *progressed = true;
            ++blocks_completed;
            if (block.boot_id != stage.boot_id) {
                pending = false;
                stage.boot_changed = true;
                stage.prior_boot_id = stage.boot_id;
                stage.boot_id = block.boot_id;
                stage.cursor_seq = block.oldest_seq;
                stage.cursor_valid = true;
                stage.overwritten_total = block.overwritten_total;
                stage.lost_internal_total = 0U;
                stage_reboot_marker(
                    &stage,
                    stage.prior_boot_id,
                    stage.boot_id,
                    (uint32_t)io->now_ms(io->context));
                break;
            }
            if (!stage_gap_counted &&
                ((block.flags & NRF_LOG_BLOCK_FLAG_GAP) != 0U ||
                 (block.record_count != 0U && block.records[0].seq != stage.cursor_seq))) {
                stage.wire_gap_increments = saturating_add(stage.wire_gap_increments, 1U);
                stage_gap_counted = true;
            }
            if (block.overwritten_total > stage.overwritten_total) {
                stage.overwritten_total = block.overwritten_total;
            }
            const uint32_t rx_uptime_ms = (uint32_t)io->now_ms(io->context);
            for (uint8_t record_index = 0U; record_index < block.record_count; ++record_index) {
                stage_record(&stage, block.boot_id, &block.records[record_index], rx_uptime_ms);
            }
            stage.cursor_seq = block.next_cursor;
            if ((block.flags & NRF_LOG_BLOCK_FLAG_MORE) == 0U || block.record_count == 0U ||
                blocks_completed == 2U) pending = false;
            if (*yielded || !pending) break;
        }
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        if (collector_lock(collector)) {
            collector->nodes[node_index].logs_unsupported = true;
            collector_unlock(collector);
        }
    }

    if (stage.count != 0U || status_valid) {
        if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
        nrf_log_node_state_t *state = &collector->nodes[node_index];
        if (!same_assignment(&old_state, state)) {
            /* sync_targets reassigned the slot during the bus I/O; the stage
             * belongs to the previous node, so it must not land in this store. */
            collector_unlock(collector);
            return ESP_OK;
        }
        state->pending_status_valid = pending && err == ESP_OK;
        state->pending_status = status;
        state->pending_status_ms = old_state.pending_status_ms;
        state->pending_blocks = blocks_completed;
        state->pending_gap_counted = stage_gap_counted;
        state->boot_id = stage.boot_id;
        state->boot_id_valid = true;
        state->cursor_seq = stage.cursor_seq;
        state->cursor_valid = stage.cursor_valid;
        state->overwritten_total = stage.overwritten_total;
        state->lost_internal_total = stage.lost_internal_total;
        state->wire_gap_total = saturating_add(state->wire_gap_total, stage.wire_gap_increments);
        if (stage.boot_changed) {
            state->logs_unsupported = false;
            state->firmware_reported = false;
        }
        const bool report_firmware = firmware_needed && firmware_exchange_completed &&
            firmware.boot_id == stage.boot_id;
        if (report_firmware) state->firmware_reported = true;
        for (uint8_t i = 0U; i < stage.count; ++i) append_record_locked(collector, node_index, &stage.records[i]);
        collector_unlock(collector);
        if (report_firmware && io->report_firmware != NULL) {
            io->report_firmware(io->context, node_index, stage.boot_id,
                firmware_supported ? &firmware : NULL);
        }
        if (stage.count != 0U && io->report_records != NULL) {
            io->report_records(io->context, node_index, stage.records, stage.count);
        }
    }
    return err;
}

nrf_log_collector_t *nrf_log_collector_create(void)
{
    runtime_observability_log_memory("log_collector_before_alloc");
    nrf_log_collector_t *collector = heap_caps_calloc(
        1U,
        sizeof(*collector),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (collector == NULL) {
        runtime_observability_log_memory("log_collector_alloc_failed");
        return NULL;
    }
    runtime_observability_log_memory("log_collector_before_mutex");
    collector->mutex = xSemaphoreCreateMutex();
    if (collector->mutex == NULL) {
        runtime_observability_log_memory("log_collector_mutex_failed");
        heap_caps_free(collector);
        return NULL;
    }
    return collector;
}

void nrf_log_collector_destroy(nrf_log_collector_t *collector)
{
    if (collector == NULL) return;
    if (collector->mutex != NULL) vSemaphoreDelete(collector->mutex);
    heap_caps_free(collector);
}

esp_err_t nrf_log_collector_reset(nrf_log_collector_t *collector)
{
    if (collector == NULL) return ESP_ERR_INVALID_ARG;
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    memset(collector->nodes, 0, sizeof(collector->nodes));
    memset(collector->records, 0, sizeof(collector->records));
    collector->next_node_index = 0U;
    collector->poll_next_start_index = 0U;
    collector->poll_in_progress = false;
    collector->poll_visited_mask = 0U;
    collector->has_polled = false;
    collector->last_poll_ms = 0U;
    collector_unlock(collector);
    return ESP_OK;
}

esp_err_t nrf_log_collector_sync_targets(
    nrf_log_collector_t *collector,
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT])
{
    if (collector == NULL || targets == NULL) return ESP_ERR_INVALID_ARG;
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    for (uint8_t i = 0U; i < NRF_LOG_COLLECTOR_NODE_COUNT; ++i) {
        nrf_log_node_state_t *state = &collector->nodes[i];
        const nrf_log_poll_target_t *target = &targets[i];
        if (target->hardware_id_length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) {
            collector_unlock(collector);
            return ESP_ERR_INVALID_ARG;
        }
        if (target->present && state->identity_valid && !identity_equal(state, target)) {
            memset(state, 0, sizeof(*state));
            memset(collector->records[i], 0, sizeof(collector->records[i]));
        }
        if (target->present) {
            if (target->link_ready && !state->link_ready) {
                state->logs_unsupported = false;
                state->backoff_rounds = 0U;
                state->backoff_remaining = 0U;
            }
            state_set_identity(state, target);
        }
        state->present = target->present;
        state->link_ready = target->present && target->link_ready;
    }
    collector_unlock(collector);
    return ESP_OK;
}

static uint8_t next_routable_node(
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT],
    uint8_t start)
{
    for (uint8_t offset = 0U; offset < NRF_LOG_COLLECTOR_NODE_COUNT; ++offset) {
        const uint8_t node = (uint8_t)((start + offset) % NRF_LOG_COLLECTOR_NODE_COUNT);
        if (targets[node].present && targets[node].route_available &&
            targets[node].link_ready) return node;
    }
    return NRF_LOG_COLLECTOR_NODE_COUNT;
}

static uint8_t next_unvisited_node(
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT],
    uint16_t visited_mask,
    uint8_t start)
{
    for (uint8_t offset = 0U; offset < NRF_LOG_COLLECTOR_NODE_COUNT; ++offset) {
        const uint8_t node = (uint8_t)((start + offset) % NRF_LOG_COLLECTOR_NODE_COUNT);
        if ((visited_mask & (uint16_t)(1U << node)) == 0U &&
            targets[node].present && targets[node].route_available && targets[node].link_ready) {
            return node;
        }
    }
    return NRF_LOG_COLLECTOR_NODE_COUNT;
}

esp_err_t nrf_log_collector_poll_iteration(
    nrf_log_collector_t *collector,
    const nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT],
    const nrf_log_collector_io_t *io,
    uint32_t per_node_budget_ms,
    uint32_t total_budget_ms,
    nrf_log_collector_iteration_t *iteration)
{
    uint64_t iteration_started_ms;
    uint8_t first_node;
    uint8_t resume_node;
    uint8_t poll_next_start_index;
    uint16_t visited_mask;
    bool poll_in_progress;
    if (collector == NULL || targets == NULL || io == NULL || iteration == NULL ||
        io->begin_node == NULL || io->end_node == NULL || io->write_frame == NULL ||
        io->read_frame == NULL || io->now_ms == NULL || io->set_deadline == NULL ||
        io->exchange_timeout_ms == 0U ||
        per_node_budget_ms == 0U || total_budget_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(iteration, 0, sizeof(*iteration));
    const uint64_t minimum_ms = (uint64_t)io->exchange_timeout_ms +
        io->mux_close_reserve_ms + io->scheduler_reserve_ms + io->session_open_timeout_ms;
    if (per_node_budget_ms < minimum_ms || total_budget_ms < minimum_ms) {
        return ESP_ERR_INVALID_ARG;
    }
    iteration->completed = true;
    iteration_started_ms = io->now_ms(io->context);
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    if (collector->has_polled && !collector->poll_in_progress &&
        iteration_started_ms >= collector->last_poll_ms &&
        iteration_started_ms - collector->last_poll_ms < NRF_LOG_COLLECTOR_PERIOD_MS) {
        collector_unlock(collector);
        return ESP_OK;
    }
    poll_in_progress = collector->poll_in_progress;
    visited_mask = collector->poll_visited_mask;
    poll_next_start_index = collector->poll_next_start_index;
    if (poll_in_progress) {
        first_node = next_unvisited_node(targets, visited_mask, collector->next_node_index);
    } else {
        first_node = next_routable_node(targets, collector->next_node_index);
        if (first_node < NRF_LOG_COLLECTOR_NODE_COUNT) {
            uint8_t next_start = next_routable_node(
                targets, (uint8_t)((first_node + 1U) % NRF_LOG_COLLECTOR_NODE_COUNT));
            if (next_start >= NRF_LOG_COLLECTOR_NODE_COUNT) next_start = first_node;
            collector->poll_in_progress = true;
            collector->poll_visited_mask = 0U;
            collector->poll_next_start_index = next_start;
            poll_next_start_index = next_start;
            poll_in_progress = true;
            visited_mask = 0U;
        }
    }
    resume_node = first_node;
    if (first_node >= NRF_LOG_COLLECTOR_NODE_COUNT && poll_in_progress) {
        collector->poll_in_progress = false;
        collector->poll_visited_mask = 0U;
        collector->next_node_index = collector->poll_next_start_index;
        collector->has_polled = true;
        collector->last_poll_ms = iteration_started_ms;
    }
    collector_unlock(collector);

    if (first_node >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        iteration->elapsed_ms = elapsed_ms(io, iteration_started_ms);
        return ESP_OK;
    }

    for (uint8_t visited = 0U; visited < NRF_LOG_COLLECTOR_NODE_COUNT; ++visited) {
        const uint8_t node_index = (uint8_t)((first_node + visited) % NRF_LOG_COLLECTOR_NODE_COUNT);
        const uint16_t node_bit = (uint16_t)(1U << node_index);
        bool attempted = false;
        bool yielded = false;
        bool progressed = false;
        bool unsupported;
        uint8_t backoff_remaining;
        esp_err_t err;
        if ((visited_mask & node_bit) != 0U) continue;
        if (!targets[node_index].present || !targets[node_index].route_available ||
            !targets[node_index].link_ready) {
            visited_mask |= node_bit;
            continue;
        }
        if (should_yield(io) || budget_limit_reached(io, iteration_started_ms, total_budget_ms)) {
            resume_node = node_index;
            iteration->completed = false;
            break;
        }
        if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
        unsupported = collector->nodes[node_index].logs_unsupported;
        backoff_remaining = collector->nodes[node_index].backoff_remaining;
        if (backoff_remaining != 0U) {
            --collector->nodes[node_index].backoff_remaining;
        }
        collector_unlock(collector);
        if (unsupported) {
            visited_mask |= node_bit;
            continue;
        }
        if (backoff_remaining != 0U) {
            visited_mask |= node_bit;
            continue;
        }
        if (targets[node_index].cs_busy) {
            ++iteration->nodes_skipped_cs;
            visited_mask |= node_bit;
            continue;
        }

        err = poll_node(
            collector,
            node_index,
            io,
            iteration_started_ms,
            per_node_budget_ms,
            total_budget_ms,
            &attempted,
            &yielded,
            &progressed);
        if (attempted) {
            ++iteration->nodes_attempted;
            if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
            nrf_log_node_state_t *state = &collector->nodes[node_index];
            if (err == ESP_OK && progressed) {
                ++iteration->nodes_succeeded;
                state->backoff_rounds = 0U;
                state->backoff_remaining = 0U;
            } else if (err != ESP_OK) {
                ++iteration->nodes_failed;
                if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
                    const uint16_t doubled = state->backoff_rounds == 0U
                        ? 1U
                        : (uint16_t)state->backoff_rounds * 2U;
                    const uint8_t maximum = CONFIG_NRF_LOG_POLL_BACKOFF_MAX_ROUNDS > UINT8_MAX
                        ? UINT8_MAX
                        : (uint8_t)CONFIG_NRF_LOG_POLL_BACKOFF_MAX_ROUNDS;
                    state->backoff_rounds = doubled > maximum ? maximum : (uint8_t)doubled;
                    state->backoff_remaining = state->backoff_rounds;
                }
            }
            collector_unlock(collector);
        }
        if (yielded || (should_yield(io) && err != ESP_OK)) {
            const uint8_t next_unvisited = next_unvisited_node(
                targets, visited_mask,
                (uint8_t)((node_index + 1U) % NRF_LOG_COLLECTOR_NODE_COUNT));
            if (next_unvisited < NRF_LOG_COLLECTOR_NODE_COUNT) {
                resume_node = next_unvisited;
            } else {
                visited_mask = 0U;
                resume_node = next_routable_node(
                    targets, (uint8_t)((node_index + 1U) % NRF_LOG_COLLECTOR_NODE_COUNT));
                poll_next_start_index = resume_node;
            }
            iteration->completed = false;
            break;
        }
        visited_mask |= node_bit;
        if (should_yield(io)) {
            const uint8_t next_unvisited = next_unvisited_node(
                targets, visited_mask,
                (uint8_t)((node_index + 1U) % NRF_LOG_COLLECTOR_NODE_COUNT));
            if (next_unvisited < NRF_LOG_COLLECTOR_NODE_COUNT) {
                resume_node = next_unvisited;
                iteration->completed = false;
                break;
            }
        }
    }
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    if (iteration->completed) {
        collector->poll_in_progress = false;
        collector->poll_visited_mask = 0U;
        collector->next_node_index = collector->poll_next_start_index;
        collector->has_polled = true;
        collector->last_poll_ms = iteration_started_ms;
    } else {
        collector->poll_visited_mask = visited_mask;
        collector->next_node_index = resume_node;
        collector->poll_next_start_index = poll_next_start_index;
    }
    collector_unlock(collector);
    iteration->elapsed_ms = elapsed_ms(io, iteration_started_ms);
    return ESP_OK;
}

esp_err_t nrf_log_collector_get_summary(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    nrf_log_node_summary_t *summary)
{
    if (collector == NULL || summary == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    const nrf_log_node_state_t *state = &collector->nodes[node_index];
    memset(summary, 0, sizeof(*summary));
    summary->present = state->present;
    summary->logs_unsupported = state->logs_unsupported;
    summary->boot_id_valid = state->boot_id_valid;
    summary->boot_id = state->boot_id;
    summary->stored = state->count;
    summary->overwritten_total = state->overwritten_total;
    summary->lost_internal_total = state->lost_internal_total;
    summary->wire_gap_total = state->wire_gap_total;
    summary->local_drop_total = state->local_drop_total;
    const uint8_t oldest = (uint8_t)((state->head + NRF_LOG_COLLECTOR_RECORD_CAPACITY - state->count) %
                                     NRF_LOG_COLLECTOR_RECORD_CAPACITY);
    for (uint16_t i = 0U; i < state->count; ++i) {
        const uint8_t slot = (uint8_t)((oldest + i) % NRF_LOG_COLLECTOR_RECORD_CAPACITY);
        const uint8_t severity = collector->records[node_index][slot].severity;
        if (severity == NRF_LOG_SEVERITY_WARN) ++summary->warn_count;
        else if (severity == NRF_LOG_SEVERITY_ERROR) ++summary->error_count;
        else if (severity == NRF_LOG_SEVERITY_FATAL) ++summary->fatal_count;
    }
    collector_unlock(collector);
    return ESP_OK;
}

esp_err_t nrf_log_collector_get_page(
    nrf_log_collector_t *collector,
    uint8_t node_index,
    bool has_after,
    uint32_t after_boot_id,
    uint32_t after_seq,
    uint16_t limit,
    nrf_log_node_page_t *page)
{
    uint16_t start = 0U;
    uint16_t copy_count;
    uint8_t oldest;
    bool after_found = !has_after;
    if (collector == NULL || page == NULL || node_index >= NRF_LOG_COLLECTOR_NODE_COUNT ||
        limit == 0U || limit > NRF_LOG_COLLECTOR_PAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!collector_lock(collector)) return ESP_ERR_TIMEOUT;
    const nrf_log_node_state_t *state = &collector->nodes[node_index];
    memset(page, 0, sizeof(*page));
    oldest = (uint8_t)((state->head + NRF_LOG_COLLECTOR_RECORD_CAPACITY - state->count) %
                       NRF_LOG_COLLECTOR_RECORD_CAPACITY);
    if (has_after) {
        for (uint16_t i = 0U; i < state->count; ++i) {
            const uint8_t slot = (uint8_t)((oldest + i) % NRF_LOG_COLLECTOR_RECORD_CAPACITY);
            const nrf_log_stored_record_t *record = &collector->records[node_index][slot];
            if (record->boot_id == after_boot_id && record->seq == after_seq) {
                start = (uint16_t)(i + 1U);
                after_found = true;
                break;
            }
        }
        if (!after_found) {
            page->reset = true;
            start = 0U;
        }
    }
    copy_count = state->count > start ? (uint16_t)(state->count - start) : 0U;
    if (copy_count > limit) copy_count = limit;
    for (uint16_t i = 0U; i < copy_count; ++i) {
        const uint8_t slot = (uint8_t)((oldest + start + i) % NRF_LOG_COLLECTOR_RECORD_CAPACITY);
        page->records[i] = collector->records[node_index][slot];
    }
    page->count = copy_count;
    page->summary.present = state->present;
    page->summary.logs_unsupported = state->logs_unsupported;
    page->summary.boot_id_valid = state->boot_id_valid;
    page->summary.boot_id = state->boot_id;
    page->summary.stored = state->count;
    page->summary.overwritten_total = state->overwritten_total;
    page->summary.lost_internal_total = state->lost_internal_total;
    page->summary.wire_gap_total = state->wire_gap_total;
    page->summary.local_drop_total = state->local_drop_total;
    for (uint16_t i = 0U; i < state->count; ++i) {
        const uint8_t slot = (uint8_t)((oldest + i) % NRF_LOG_COLLECTOR_RECORD_CAPACITY);
        const uint8_t severity = collector->records[node_index][slot].severity;
        if (severity == NRF_LOG_SEVERITY_WARN) ++page->summary.warn_count;
        else if (severity == NRF_LOG_SEVERITY_ERROR) ++page->summary.error_count;
        else if (severity == NRF_LOG_SEVERITY_FATAL) ++page->summary.fatal_count;
    }
    if (copy_count != 0U) {
        page->next_token_valid = true;
        page->next_token_boot_id = page->records[copy_count - 1U].boot_id;
        page->next_token_seq = page->records[copy_count - 1U].seq;
    } else if (has_after && after_found) {
        page->next_token_valid = true;
        page->next_token_boot_id = after_boot_id;
        page->next_token_seq = after_seq;
    }
    collector_unlock(collector);
    return ESP_OK;
}
