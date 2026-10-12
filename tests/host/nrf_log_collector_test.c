#include "nrf_log_collector.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nrf_log_protocol.h"
#include "nrf_log_ring.h"
#include "nrf_topology_periods.h"
#include "nrf_upstream_protocol.h"
#include "sensor_bus_errors.h"

#define FAKE_EVENT_CAPACITY 32768U
#define ROUND_MS NRF_LOG_COLLECTOR_PERIOD_MS

typedef enum {
    EVENT_SELECT = 1,
    EVENT_SELECT_VERIFY,
    EVENT_WRITE,
    EVENT_READ,
    EVENT_CLOSE,
    EVENT_CLOSE_VERIFY,
    EVENT_RECOVERY,
    EVENT_CHANNEL_CHANGE,
    EVENT_NODE_RESET,
    EVENT_RECONCILE,
    EVENT_SHUTDOWN,
    EVENT_PRIORITY_STORM,
} fake_event_kind_t;

typedef struct {
    fake_event_kind_t kind;
    uint8_t node;
    uint8_t message_type;
    uint8_t physical_node;
    uint8_t mux_channel;
} fake_event_t;

typedef enum {
    RESET_INJECTION_NONE = 0,
    RESET_AFTER_STATUS_READ,
    RESET_AFTER_FIRST_BLOCK_READ,
} reset_injection_point_t;

typedef enum {
    FAULT_NONE = 0,
    FAULT_NACK_BEFORE_WRITE,
    FAULT_NACK_AFTER_WRITE,
    FAULT_TIMEOUT_BEFORE_WRITE,
    FAULT_TIMEOUT_AFTER_WRITE,
    FAULT_SHORT_RX,
    FAULT_BIT_FLIP,
    FAULT_BIT_BURST,
    FAULT_BAD_STATUS_SEMANTICS,
    FAULT_BAD_BLOCK_SEMANTICS,
    FAULT_OTHER_BOOT_ID,
    FAULT_STALE_REQUEST_ID,
    FAULT_MALFORMED_SUCCESS,
    FAULT_UNKNOWN_MESSAGE,
    FAULT_NRF_BUSY,
    FAULT_RESET_DURING_RX,
    FAULT_RESET_ON_SECOND_BLOCK,
    FAULT_CORRUPT_FOREVER,
    FAULT_CHANNEL_CHANGED,
} fake_fault_t;

typedef struct {
    nrf_log_ring_t rings[NRF_LOG_COLLECTOR_NODE_COUNT];
    nrf_log_poll_target_t targets[NRF_LOG_COLLECTOR_NODE_COUNT];
    fake_event_t events[FAKE_EVENT_CAPACITY];
    uint8_t pending[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t pending_length;
    uint8_t pending_type;
    uint8_t active_node;
    uint8_t active_physical_node;
    uint8_t active_mux_channel;
    uint8_t route_physical_node[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint8_t fault_node;
    uint8_t fault_response_type;
    uint8_t other_channel;
    fake_fault_t fault;
    bool fault_always;
    bool fault_used;
    bool yield_pending;
    bool shutdown_requested;
    bool shutdown_after_first_block_read;
    bool shutdown_injected;
    bool yield_after_status_read;
    uint8_t yield_after_status_node;
    bool deadlines_enabled;
    uint64_t cs_deadline_ms;
    uint64_t sensor_deadline_ms;
    uint64_t reconcile_deadline_ms;
    uint32_t cs_deadlines_served;
    uint32_t sensor_deadlines_served;
    uint32_t reconcile_deadlines_served;
    uint32_t cs_deadline_max_lateness_ms;
    uint32_t sensor_deadline_max_lateness_ms;
    uint32_t reconcile_deadline_max_lateness_ms;
    bool sensor_refresh_active;
    bool cs_command_active;
    bool priority_storm_after_status_read;
    bool priority_storm_injected;
    uint8_t priority_storm_node;
    reset_injection_point_t reset_injection_point;
    uint8_t reset_injection_node;
    uint32_t reset_block_read_target;
    bool node_reset_injected;
    uint32_t reset_old_boot_id;
    uint32_t reset_new_boot_id;
    bool reconcile_on_first_block_write;
    bool reconcile_injected;
    uint8_t reconcile_node;
    uint8_t reconcile_new_physical_node;
    uint8_t reconcile_new_mux_channel;
    uint32_t mux_selects;
    uint32_t mux_closes;
    bool mux_open;
    esp_err_t begin_node_error;
    bool inject_page_read_during_write;
    bool readback_mismatch;
    bool select_failure;
    bool close_failure;
    uint8_t select_failure_node;
    uint8_t close_failure_node;
    bool old_firmware[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t last_status_id[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t last_block_id[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t last_block_cursor[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t last_block_boot[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint8_t last_block_max_records[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t next_uptime[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t status_writes[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t block_writes[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t status_reads[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t block_reads[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t duplicate_deliveries[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t last_write_id[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint8_t last_write_type[NRF_LOG_COLLECTOR_NODE_COUNT];
    uint32_t recoveries;
    uint32_t tx_cost_ms;
    uint64_t now_ms;
    size_t event_count;
    nrf_log_collector_t *collector;
} fake_bus_t;

static fake_bus_t s_fake;
static uint32_t s_assertions;
static uint32_t active_seed;
static uint32_t s_production_defects;

#define CHECK(condition) do { \
    ++s_assertions; \
    if (!(condition)) { \
        fprintf(stderr, "seed=0x%08lX failed at %s:%d: %s\n", \
            (unsigned long)active_seed, __FILE__, __LINE__, #condition); \
        assert(condition); \
    } \
} while (0)

#define CHECK_PRODUCTION_INVARIANT(condition, message) do { \
    ++s_assertions; \
    if (!(condition)) { \
        ++s_production_defects; \
        fprintf(stderr, "PRODUCTION DEFECT: %s (seed=0x%08lX, %s:%d: %s)\n", \
            message, (unsigned long)active_seed, __FILE__, __LINE__, #condition); \
    } \
} while (0)

typedef struct {
    nrf_log_node_summary_t summary;
    nrf_log_node_page_t page;
    uint32_t cursor_seq;
} collector_state_snapshot_t;

static nrf_log_lock_key_t fake_ring_lock(void *context)
{
    (void)context;
    return 0U;
}

static void fake_ring_unlock(void *context, nrf_log_lock_key_t key)
{
    (void)context;
    (void)key;
}

static uint32_t fake_next_boot_id(void *context)
{
    uint32_t *boot_id = context;
    return *boot_id + 1U;
}

static void fake_event(fake_bus_t *fake, fake_event_kind_t kind, uint8_t node, uint8_t type)
{
    CHECK(fake->event_count < FAKE_EVENT_CAPACITY);
    fake->events[fake->event_count++] = (fake_event_t) {
        .kind = kind,
        .node = node,
        .message_type = type,
        .physical_node = fake->active_physical_node,
        .mux_channel = fake->active_mux_channel,
    };
}

static bool fake_take_fault(fake_bus_t *fake, uint8_t node, uint8_t response_type)
{
    if (fake->fault == FAULT_NONE || node != fake->fault_node ||
        (fake->fault_response_type != 0U && fake->fault_response_type != response_type) ||
        (!fake->fault_always && fake->fault_used)) {
        return false;
    }
    fake->fault_used = true;
    return true;
}

static void fake_recover(fake_bus_t *fake, uint8_t node)
{
    ++fake->recoveries;
    fake_event(fake, EVENT_RECOVERY, node, 0U);
    fake->now_ms += 1U;
}

static void fake_repair_crc(uint8_t *frame, size_t length)
{
    const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(
        frame, length - NRF_LOG_PROTOCOL_CRC_LENGTH);
    frame[length - 2U] = (uint8_t)crc;
    frame[length - 1U] = (uint8_t)(crc >> 8U);
}

static void fake_write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static esp_err_t fake_init_ring(fake_bus_t *fake, uint8_t node, uint32_t boot_id);
static void fake_append(fake_bus_t *fake, uint8_t node, uint32_t count);

static void fake_reset_active_ring(fake_bus_t *fake, uint8_t node, uint8_t response_type)
{
    uint8_t physical_node = fake->active_physical_node;
    bool inject = false;
    if (physical_node >= NRF_LOG_COLLECTOR_NODE_COUNT) physical_node = node;
    if (fake->node_reset_injected || node != fake->reset_injection_node) return;
    if (fake->reset_injection_point == RESET_AFTER_STATUS_READ &&
        response_type == NRF_LOG_MESSAGE_STATUS) {
        inject = true;
    } else if (fake->reset_injection_point == RESET_AFTER_FIRST_BLOCK_READ &&
               response_type == NRF_LOG_MESSAGE_BLOCK &&
               fake->block_reads[node] == fake->reset_block_read_target) {
        inject = true;
    }
    if (!inject) return;

    CHECK(fake->active_node == node && fake->mux_open);
    fake->reset_old_boot_id = fake->rings[physical_node].boot_id;
    fake->reset_new_boot_id = fake->reset_old_boot_id + 1U;
    if (fake->reset_new_boot_id == 0U) fake->reset_new_boot_id = 1U;
    fake_event(fake, EVENT_NODE_RESET, node, response_type);
    CHECK(fake_init_ring(fake, physical_node, fake->reset_new_boot_id) == ESP_OK);
    fake_append(fake, physical_node, 1U);
    fake->node_reset_injected = true;
}

static void fake_inject_priority_storm(fake_bus_t *fake, uint8_t node)
{
    if (!fake->priority_storm_after_status_read || fake->priority_storm_injected ||
        node != fake->priority_storm_node) return;
    CHECK(fake->active_node == node && fake->mux_open);
    fake->priority_storm_injected = true;
    fake->sensor_refresh_active = true;
    fake->cs_command_active = true;
    /* Mirrors service_log_build_targets() deriving cs_busy from command_active. */
    fake->targets[1U].cs_busy = true;
    fake->targets[2U].cs_busy = true;
    fake->deadlines_enabled = true;
    fake->cs_deadline_ms = fake->now_ms;
    fake->sensor_deadline_ms = fake->now_ms;
    fake->reconcile_deadline_ms = fake->now_ms;
    for (uint8_t storm_node = 1U; storm_node <= 4U; ++storm_node) {
        fake_append(fake, storm_node, storm_node == node ? 16U : 2U);
    }
    fake_event(fake, EVENT_PRIORITY_STORM, node, NRF_LOG_MESSAGE_STATUS);
}

static void fake_reconcile_during_block_write(fake_bus_t *fake, uint8_t node)
{
    nrf_log_poll_target_t *target;
    if (!fake->reconcile_on_first_block_write || fake->reconcile_injected ||
        node != fake->reconcile_node) return;
    CHECK(fake->active_node == node && fake->mux_open);
    target = &fake->targets[node];
    target->node_id = (uint8_t)(UINT8_C(0x40) + node);
    target->hardware_id[0] = (uint8_t)(UINT8_C(0xD0) + node);
    ++target->assignment_generation;
    target->mux_channel = fake->reconcile_new_mux_channel;
    fake->route_physical_node[node] = fake->reconcile_new_physical_node;
    CHECK(nrf_log_collector_sync_targets(fake->collector, fake->targets) == ESP_OK);
    fake->reconcile_injected = true;
    fake_event(fake, EVENT_RECONCILE, node, NRF_LOG_MESSAGE_READ_REQUEST);
}

static esp_err_t fake_init_ring(fake_bus_t *fake, uint8_t node, uint32_t boot_id)
{
    const nrf_log_lock_interface_t lock = {
        .lock = fake_ring_lock,
        .unlock = fake_ring_unlock,
        .context = NULL,
    };
    const nrf_log_error_t result = nrf_log_ring_init(
        &fake->rings[node],
        boot_id,
        0U,
        &lock,
        fake_next_boot_id,
        &fake->rings[node].boot_id);
    fake->next_uptime[node] = 1U;
    return result == NRF_LOG_ERROR_OK ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void fake_append(fake_bus_t *fake, uint8_t node, uint32_t count)
{
    nrf_log_ring_t *ring = &fake->rings[node];
    uint32_t uptime = fake->next_uptime[node];
    for (uint32_t i = 0U; i < count; ++i) {
        CHECK(nrf_log_ring_append(
            ring,
            NRF_LOG_SUBSYSTEM_SENSOR,
            NRF_LOG_SEVERITY_INFO,
            UINT16_C(0x0101),
            node,
            i,
            uptime++) == NRF_LOG_ERROR_OK);
    }
    fake->next_uptime[node] = uptime;
}

static void fake_set_targets(fake_bus_t *fake, uint8_t present_count)
{
    for (uint8_t node = 0U; node < NRF_LOG_COLLECTOR_NODE_COUNT; ++node) {
        nrf_log_poll_target_t *target = &fake->targets[node];
        memset(target, 0, sizeof(*target));
        target->present = node < present_count;
        target->route_available = true;
        target->node_id = node == 0U ? NRF_TOPOLOGY_MAIN_NODE_ID : node;
        target->mux_channel = node == 0U ? 0U : (uint8_t)(node - 1U);
        target->current_address = node == 0U
            ? NRF_UPSTREAM_ADDRESS_MAIN
            : NRF_UPSTREAM_ADDRESS_REMOTE;
        target->hardware_id_length = 4U;
        target->hardware_id[0] = (uint8_t)(node + 1U);
        target->hardware_id[1] = 0xA5U;
        target->hardware_id[2] = 0x5AU;
        target->hardware_id[3] = 0xC3U;
        target->assignment_generation = 1U;
    }
    CHECK(nrf_log_collector_sync_targets(fake->collector, fake->targets) == ESP_OK);
}

static void fake_start(uint8_t present_count)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.tx_cost_ms = 1U;
    s_fake.now_ms = 100U;
    s_fake.collector = nrf_log_collector_create();
    CHECK(s_fake.collector != NULL);
    for (uint8_t node = 0U; node < NRF_LOG_COLLECTOR_NODE_COUNT; ++node) {
        s_fake.route_physical_node[node] = node;
        CHECK(fake_init_ring(&s_fake, node, UINT32_C(0x1000) + node) == ESP_OK);
    }
    s_fake.active_node = UINT8_MAX;
    s_fake.active_physical_node = UINT8_MAX;
    s_fake.active_mux_channel = UINT8_MAX;
    fake_set_targets(&s_fake, present_count);
}

static void fake_stop(void)
{
    nrf_log_collector_destroy(s_fake.collector);
    s_fake.collector = NULL;
}

static esp_err_t fake_begin_node(void *context, uint8_t node)
{
    fake_bus_t *fake = context;
    if (fake->begin_node_error != ESP_OK) {
        const esp_err_t result = fake->begin_node_error;
        fake->begin_node_error = ESP_OK;
        return result;
    }
    fake->active_node = node;
    fake->active_physical_node = fake->route_physical_node[node];
    fake->active_mux_channel = fake->targets[node].mux_channel;
    if (node != 0U) {
        /* Only the 10.4 reconcile scenario may move a node to another channel;
         * everywhere else the one-hot mapping stays node N -> channel N-1. */
        CHECK(fake->reconcile_injected
            ? fake->targets[node].mux_channel < NRF_TOPOLOGY_REMOTE_COUNT
            : fake->targets[node].mux_channel == (uint8_t)(node - 1U));
        fake_event(fake, EVENT_CLOSE, node, 0U);
        fake_event(fake, EVENT_CLOSE_VERIFY, node, 0U);
        fake_event(fake, EVENT_SELECT, node, 0U);
        fake->now_ms += 1U;
        if (fake->select_failure && fake->select_failure_node == node) {
            fake_event(fake, EVENT_CLOSE, node, 0U);
            fake_event(fake, EVENT_CLOSE_VERIFY, node, 0U);
            fake->active_node = UINT8_MAX;
            fake->active_physical_node = UINT8_MAX;
            fake->active_mux_channel = UINT8_MAX;
            return ESP_ERR_INVALID_RESPONSE;
        }
        fake->mux_open = true;
        ++fake->mux_selects;
        fake_event(fake, EVENT_SELECT_VERIFY, node, 0U);
        if (fake->readback_mismatch && fake->select_failure_node == node) {
            fake_event(fake, EVENT_CLOSE, node, 0U);
            fake_event(fake, EVENT_CLOSE_VERIFY, node, 0U);
            fake->mux_open = false;
            ++fake->mux_closes;
            fake->active_node = UINT8_MAX;
            fake->active_physical_node = UINT8_MAX;
            fake->active_mux_channel = UINT8_MAX;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static esp_err_t fake_end_node(void *context, uint8_t node)
{
    fake_bus_t *fake = context;
    if (node == 0U) return ESP_OK;
    fake_event(fake, EVENT_CLOSE, node, 0U);
    fake_event(fake, EVENT_CLOSE_VERIFY, node, 0U);
    fake->now_ms += 1U;
    if (fake->close_failure && fake->close_failure_node == node) {
        fake_event(fake, EVENT_CLOSE, node, 0U);
        fake_event(fake, EVENT_CLOSE_VERIFY, node, 0U);
        fake->now_ms += 1U;
        fake->mux_open = false;
        ++fake->mux_closes;
        fake->active_node = UINT8_MAX;
        fake->active_physical_node = UINT8_MAX;
        fake->active_mux_channel = UINT8_MAX;
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (fake->mux_open) ++fake->mux_closes;
    fake->mux_open = false;
    fake->active_node = UINT8_MAX;
    fake->active_physical_node = UINT8_MAX;
    fake->active_mux_channel = UINT8_MAX;
    return ESP_OK;
}

static esp_err_t fake_build_legacy_status(fake_bus_t *fake, uint8_t node, uint32_t request_id)
{
    nrf_upstream_status_t status = {0};
    status.request_id = request_id;
    status.hardware_id_length = fake->targets[node].hardware_id_length;
    memcpy(status.hardware_id, fake->targets[node].hardware_id, status.hardware_id_length);
    status.role = node == 0U ? NRF_UPSTREAM_ROLE_MAIN : NRF_UPSTREAM_ROLE_REMOTE;
    status.node_id = fake->targets[node].node_id;
    status.mux_channel = node == 0U ? NRF_UPSTREAM_MUX_CHANNEL_NONE : (uint8_t)(node - 1U);
    status.target_address = node == 0U ? NRF_UPSTREAM_ADDRESS_MAIN : NRF_UPSTREAM_ADDRESS_REMOTE;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    if (nrf_upstream_protocol_encode_status(&status, fake->pending) != NRF_UPSTREAM_RESULT_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    fake->pending_length = NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH;
    fake->pending_type = NRF_LOG_MESSAGE_STATUS;
    return ESP_OK;
}

static esp_err_t fake_write_frame(
    void *context,
    uint8_t node,
    const uint8_t *frame,
    size_t frame_length)
{
    fake_bus_t *fake = context;
    const uint8_t request_type = frame_length > 3U ? frame[3] : 0U;
    uint8_t physical_node = fake->active_physical_node;
    const uint8_t response_type = request_type == NRF_LOG_MESSAGE_STATUS_REQUEST
        ? NRF_LOG_MESSAGE_STATUS
        : NRF_LOG_MESSAGE_BLOCK;
    bool fault = false;
    esp_err_t result;
    if (node != 0U && fake->active_node != node) return ESP_ERR_INVALID_STATE;
    if (physical_node >= NRF_LOG_COLLECTOR_NODE_COUNT) physical_node = node;
    fake_event(fake, EVENT_WRITE, node, request_type);
    fake->now_ms += fake->tx_cost_ms;
    if (request_type == NRF_LOG_MESSAGE_STATUS_REQUEST) ++fake->status_writes[node];
    else if (request_type == NRF_LOG_MESSAGE_READ_REQUEST) {
        ++fake->block_writes[node];
        if (fake->block_writes[node] == 1U) fake_reconcile_during_block_write(fake, node);
    }
    if (fake->inject_page_read_during_write) {
        nrf_log_node_page_t page;
        fake->inject_page_read_during_write = false;
        CHECK(nrf_log_collector_get_page(fake->collector, node, false, 0U, 0U, 1U, &page) == ESP_OK);
    }
    if (fake->last_write_id[node] == (uint32_t)frame[6] +
            ((uint32_t)frame[7] << 8U) + ((uint32_t)frame[8] << 16U) + ((uint32_t)frame[9] << 24U) &&
        fake->last_write_type[node] == request_type) {
        ++fake->duplicate_deliveries[node];
    }
    fake->last_write_id[node] = (uint32_t)frame[6] +
        ((uint32_t)frame[7] << 8U) + ((uint32_t)frame[8] << 16U) + ((uint32_t)frame[9] << 24U);
    fake->last_write_type[node] = request_type;

    if (fake->fault == FAULT_NACK_BEFORE_WRITE || fake->fault == FAULT_NACK_AFTER_WRITE ||
        fake->fault == FAULT_TIMEOUT_BEFORE_WRITE || fake->fault == FAULT_TIMEOUT_AFTER_WRITE) {
        fault = fake_take_fault(fake, node, response_type);
    }
    if (fault && fake->fault == FAULT_NACK_BEFORE_WRITE) return ESP_ERR_NOT_FOUND;
    if (fault && fake->fault == FAULT_TIMEOUT_BEFORE_WRITE) {
        fake_recover(fake, node);
        return ESP_ERR_TIMEOUT;
    }

    if (request_type == NRF_LOG_MESSAGE_STATUS_REQUEST) {
        nrf_log_status_request_t request;
        if (nrf_log_protocol_decode_status_request(frame, frame_length, &request) != NRF_LOG_ERROR_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fake->last_status_id[node] = request.request_id;
        if (fake->old_firmware[node]) {
            result = fake_build_legacy_status(fake, node, request.request_id);
        } else {
            result = nrf_log_ring_build_status_frame(
                         &fake->rings[physical_node],
                         request.request_id,
                         fake->pending,
                         sizeof(fake->pending),
                         &fake->pending_length) == NRF_LOG_ERROR_OK
                ? ESP_OK
                : ESP_ERR_INVALID_RESPONSE;
            fake->pending_type = NRF_LOG_MESSAGE_STATUS;
        }
    } else if (request_type == NRF_LOG_MESSAGE_READ_REQUEST) {
        nrf_log_read_request_t request;
        if (nrf_log_protocol_decode_read_request(frame, frame_length, &request) != NRF_LOG_ERROR_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        fake->last_block_id[node] = request.request_id;
        fake->last_block_cursor[node] = request.cursor_seq;
        fake->last_block_boot[node] = request.expected_boot_id;
        fake->last_block_max_records[node] = request.max_records;
        result = nrf_log_ring_build_block_frame(
                     &fake->rings[physical_node],
                     request.request_id,
                     request.expected_boot_id,
                     request.cursor_seq,
                     request.max_records,
                     fake->pending,
                     sizeof(fake->pending),
                     &fake->pending_length) == NRF_LOG_ERROR_OK
            ? ESP_OK
            : ESP_ERR_INVALID_RESPONSE;
        fake->pending_type = NRF_LOG_MESSAGE_BLOCK;
    } else {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK) return result;

    if (fault && fake->fault == FAULT_NACK_AFTER_WRITE) return ESP_ERR_NOT_FOUND;
    if (fault && fake->fault == FAULT_TIMEOUT_AFTER_WRITE) {
        fake_recover(fake, node);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static bool fake_fault_applies(fake_bus_t *fake, uint8_t node, uint8_t response_type)
{
    return node == fake->fault_node && fake->fault != FAULT_NONE &&
        (fake->fault_response_type == 0U || fake->fault_response_type == response_type);
}

static esp_err_t fake_make_other_channel_reply(fake_bus_t *fake, uint8_t node, uint8_t response_type)
{
    uint8_t other = fake->other_channel;
    if (other >= NRF_LOG_COLLECTOR_NODE_COUNT || other == node) {
        other = node == 1U ? 2U : 1U;
    }
    fake_event(fake, EVENT_CHANNEL_CHANGE, other, response_type);
    fake->active_node = other;
    if (response_type == NRF_LOG_MESSAGE_STATUS) {
        const uint32_t request_id = fake->last_status_id[other] != 0U
            ? fake->last_status_id[other]
            : (uint32_t)other * 4U + 1U;
        return nrf_log_ring_build_status_frame(
                   &fake->rings[fake->route_physical_node[other]], request_id,
                   fake->pending, sizeof(fake->pending), &fake->pending_length) ==
                NRF_LOG_ERROR_OK
            ? ESP_OK
            : ESP_ERR_INVALID_RESPONSE;
    }
    const uint32_t request_id = fake->last_block_id[other];
    return nrf_log_ring_build_block_frame(
               &fake->rings[fake->route_physical_node[other]],
               request_id,
               fake->last_block_boot[other],
               fake->last_block_cursor[other],
               fake->last_block_max_records[other],
               fake->pending,
               sizeof(fake->pending),
               &fake->pending_length) == NRF_LOG_ERROR_OK
        ? ESP_OK
        : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t fake_read_frame(
    void *context,
    uint8_t node,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *received_length)
{
    fake_bus_t *fake = context;
    const uint8_t response_type = fake->pending_type;
    uint8_t physical_node = fake->active_physical_node;
    bool fault;
    size_t copy_length;
    if (physical_node >= NRF_LOG_COLLECTOR_NODE_COUNT) physical_node = node;
    fake_event(fake, EVENT_READ, node, response_type);
    fake->now_ms += fake->tx_cost_ms;
    if (response_type == NRF_LOG_MESSAGE_STATUS) ++fake->status_reads[node];
    else ++fake->block_reads[node];
    if (fake->pending_length > frame_capacity) return ESP_ERR_INVALID_RESPONSE;

    fault = fake_fault_applies(fake, node, response_type) &&
        (fake->fault_always || !fake->fault_used);
    if (fake->fault == FAULT_RESET_ON_SECOND_BLOCK) {
        fault = fake_fault_applies(fake, node, response_type) &&
            response_type == NRF_LOG_MESSAGE_BLOCK && fake->block_reads[node] == 2U && !fake->fault_used;
    }
    if (fault && fake->fault != FAULT_CHANNEL_CHANGED) fake->fault_used = true;
    if (fault && fake->fault == FAULT_CHANNEL_CHANGED) {
        const esp_err_t err = fake_make_other_channel_reply(fake, node, response_type);
        if (err != ESP_OK) return err;
    }
    if (fault && (fake->fault == FAULT_RESET_DURING_RX ||
                  fake->fault == FAULT_RESET_ON_SECOND_BLOCK) &&
        response_type == NRF_LOG_MESSAGE_BLOCK) {
        uint32_t new_boot = fake->rings[physical_node].boot_id + 1U;
        if (new_boot == 0U) new_boot = 1U;
        CHECK(fake_init_ring(fake, physical_node, new_boot) == ESP_OK);
        fake_append(fake, physical_node, 1U);
        fake_recover(fake, node);
        return ESP_ERR_TIMEOUT;
    }
    if (fault && fake->fault == FAULT_NRF_BUSY) {
        fake_recover(fake, node);
        return ESP_ERR_TIMEOUT;
    }

    copy_length = fake->pending_length;
    memcpy(frame, fake->pending, copy_length);
    if (fault && fake->fault == FAULT_SHORT_RX && copy_length != 0U) {
        --copy_length;
    } else if (fault && (fake->fault == FAULT_BIT_FLIP || fake->fault == FAULT_CORRUPT_FOREVER)) {
        frame[10] ^= 0x01U;
    } else if (fault && fake->fault == FAULT_BIT_BURST) {
        frame[10] ^= 0x81U;
        frame[11] ^= 0x42U;
        frame[12] ^= 0x24U;
    } else if (fault && fake->fault == FAULT_BAD_STATUS_SEMANTICS) {
        frame[14] = 0x81U;
        frame[15] = 0x00U;
        fake_repair_crc(frame, fake->pending_length);
    } else if (fault && fake->fault == FAULT_BAD_BLOCK_SEMANTICS) {
        const uint32_t next_cursor = (uint32_t)frame[22] |
            ((uint32_t)frame[23] << 8U) |
            ((uint32_t)frame[24] << 16U) |
            ((uint32_t)frame[25] << 24U);
        fake_write_u32(&frame[22], next_cursor + 1U);
        fake_repair_crc(frame, fake->pending_length);
    } else if (fault && fake->fault == FAULT_OTHER_BOOT_ID) {
        fake_write_u32(&frame[14], fake->rings[physical_node].boot_id + 1U);
        fake_repair_crc(frame, fake->pending_length);
    } else if (fault && fake->fault == FAULT_STALE_REQUEST_ID) {
        const uint32_t request_id = (uint32_t)frame[6] |
            ((uint32_t)frame[7] << 8U) |
            ((uint32_t)frame[8] << 16U) |
            ((uint32_t)frame[9] << 24U);
        fake_write_u32(&frame[6], request_id ^ UINT32_C(0x00000100));
        fake_repair_crc(frame, fake->pending_length);
    } else if (fault && fake->fault == FAULT_MALFORMED_SUCCESS) {
        frame[10] = 0x7FU;
        fake_repair_crc(frame, fake->pending_length);
    } else if (fault && fake->fault == FAULT_UNKNOWN_MESSAGE) {
        frame[3] = 0x99U;
        fake_repair_crc(frame, fake->pending_length);
    }
    if (fake->yield_after_status_read && node == fake->yield_after_status_node &&
        response_type == NRF_LOG_MESSAGE_STATUS) {
        fake->yield_after_status_read = false;
        fake->yield_pending = true;
    }
    *received_length = copy_length;
    if (response_type == NRF_LOG_MESSAGE_STATUS) fake_inject_priority_storm(fake, node);
    if (fake->shutdown_after_first_block_read && !fake->shutdown_injected &&
        response_type == NRF_LOG_MESSAGE_BLOCK && fake->block_reads[node] == 1U) {
        /* Raise the stop predicate inside the completed read callback, before it returns. */
        CHECK(fake->active_node == node && fake->mux_open);
        fake->shutdown_injected = true;
        fake->shutdown_requested = true;
        fake_event(fake, EVENT_SHUTDOWN, node, response_type);
    }
    return ESP_OK;
}

static uint64_t fake_now(void *context)
{
    return ((fake_bus_t *)context)->now_ms;
}

static void fake_service_due_deadlines(fake_bus_t *fake);

static bool fake_yield_requested(void *context)
{
    fake_bus_t *fake = context;
    if (fake->event_count != 0U) {
        const fake_event_t *last_event = &fake->events[fake->event_count - 1U];
        if (last_event->kind == EVENT_READ && last_event->node == fake->active_node) {
            fake_reset_active_ring(fake, last_event->node, last_event->message_type);
        }
    }
    const bool deadline_due = fake->deadlines_enabled &&
        (fake->now_ms >= fake->cs_deadline_ms ||
         fake->now_ms >= fake->sensor_deadline_ms ||
         fake->now_ms >= fake->reconcile_deadline_ms);
    if (deadline_due) fake_service_due_deadlines(fake);
    /* Production maps stop/immediate work/reconcile pressure through this callback. */
    return fake->yield_pending || fake->shutdown_requested || fake->sensor_refresh_active || deadline_due;
}

static void fake_service_due_deadlines(fake_bus_t *fake)
{
    const uint64_t now = fake->now_ms;
    if (!fake->deadlines_enabled) return;
    if (now >= fake->cs_deadline_ms) {
        const uint32_t late = (uint32_t)(now - fake->cs_deadline_ms);
        if (late > fake->cs_deadline_max_lateness_ms) fake->cs_deadline_max_lateness_ms = late;
        ++fake->cs_deadlines_served;
    }
    if (now >= fake->sensor_deadline_ms) {
        const uint32_t late = (uint32_t)(now - fake->sensor_deadline_ms);
        if (late > fake->sensor_deadline_max_lateness_ms) fake->sensor_deadline_max_lateness_ms = late;
        ++fake->sensor_deadlines_served;
    }
    if (now >= fake->reconcile_deadline_ms) {
        const uint32_t late = (uint32_t)(now - fake->reconcile_deadline_ms);
        if (late > fake->reconcile_deadline_max_lateness_ms) fake->reconcile_deadline_max_lateness_ms = late;
        ++fake->reconcile_deadlines_served;
    }
    fake->deadlines_enabled = false;
}

static nrf_log_collector_io_t fake_io(fake_bus_t *fake)
{
    return (nrf_log_collector_io_t) {
        .context = fake,
        .begin_node = fake_begin_node,
        .end_node = fake_end_node,
        .write_frame = fake_write_frame,
        .read_frame = fake_read_frame,
        .now_ms = fake_now,
        .yield_requested = fake_yield_requested,
    };
}

static nrf_log_collector_iteration_t run_round_at_current_time(fake_bus_t *fake)
{
    nrf_log_collector_iteration_t iteration;
    const nrf_log_collector_io_t io = fake_io(fake);
    CHECK(nrf_log_collector_poll_iteration(
        fake->collector,
        fake->targets,
        &io,
        50U,
        100U,
        &iteration) == ESP_OK);
    return iteration;
}

static nrf_log_collector_iteration_t run_round(fake_bus_t *fake)
{
    fake->now_ms += ROUND_MS;
    return run_round_at_current_time(fake);
}

static nrf_log_node_summary_t node_summary(fake_bus_t *fake, uint8_t node)
{
    nrf_log_node_summary_t summary;
    CHECK(nrf_log_collector_get_summary(fake->collector, node, &summary) == ESP_OK);
    return summary;
}

static nrf_log_node_page_t node_page(fake_bus_t *fake, uint8_t node, bool after, uint32_t boot, uint32_t seq, uint16_t limit)
{
    nrf_log_node_page_t page;
    CHECK(nrf_log_collector_get_page(fake->collector, node, after, boot, seq, limit, &page) == ESP_OK);
    return page;
}

static collector_state_snapshot_t snapshot_node(fake_bus_t *fake, uint8_t node)
{
    collector_state_snapshot_t snapshot;
    /* Zero padding too: the snapshots are compared with memcmp. */
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(nrf_log_collector_get_summary(fake->collector, node, &snapshot.summary) == ESP_OK);
    snapshot.page = node_page(fake, node, false, 0U, 0U, NRF_LOG_COLLECTOR_RECORD_CAPACITY);
    snapshot.cursor_seq = snapshot.page.count == 0U
        ? 0U
        : snapshot.page.records[snapshot.page.count - 1U].seq + 1U;
    return snapshot;
}

static void check_snapshot_equal(
    const collector_state_snapshot_t *before,
    const collector_state_snapshot_t *after)
{
    CHECK(before->cursor_seq == after->cursor_seq);
    CHECK(memcmp(&before->summary, &after->summary, sizeof(before->summary)) == 0);
    CHECK(memcmp(&before->page, &after->page, sizeof(before->page)) == 0);
}

static void assert_record_count(fake_bus_t *fake, uint8_t node, uint16_t expected)
{
    CHECK(node_summary(fake, node).stored == expected);
}

static bool trace_event_is(
    const fake_bus_t *fake,
    size_t index,
    fake_event_kind_t kind,
    uint8_t node,
    uint8_t message_type)
{
    return index < fake->event_count && fake->events[index].kind == kind &&
        fake->events[index].node == node && fake->events[index].message_type == message_type;
}

static void test_framing_budget_and_order(void)
{
    fake_start(3U);
    s_fake.targets[0].present = false;
    s_fake.targets[1].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    fake_append(&s_fake, 2U, 11U);
    const nrf_log_collector_iteration_t result = run_round_at_current_time(&s_fake);
    CHECK(result.nodes_attempted == 1U);
    CHECK(s_fake.block_writes[2] == 2U);
    CHECK(s_fake.last_block_max_records[2] == NRF_LOG_PROTOCOL_MAX_RECORDS);
    CHECK(node_summary(&s_fake, 2U).stored == 10U);
    nrf_log_node_page_t page = node_page(&s_fake, 2U, false, 0U, 0U, 20U);
    CHECK(page.count == 10U);
    for (uint16_t i = 0U; i < page.count; ++i) CHECK(page.records[i].seq == i);
    CHECK(trace_event_is(&s_fake, 0U, EVENT_CLOSE, 2U, 0U));
    CHECK(trace_event_is(&s_fake, 1U, EVENT_CLOSE_VERIFY, 2U, 0U));
    CHECK(trace_event_is(&s_fake, 2U, EVENT_SELECT, 2U, 0U));
    CHECK(trace_event_is(&s_fake, 3U, EVENT_SELECT_VERIFY, 2U, 0U));
    CHECK(trace_event_is(&s_fake, 4U, EVENT_WRITE, 2U, NRF_LOG_MESSAGE_STATUS_REQUEST));
    CHECK(trace_event_is(&s_fake, 5U, EVENT_READ, 2U, NRF_LOG_MESSAGE_STATUS));
    CHECK(trace_event_is(&s_fake, 6U, EVENT_WRITE, 2U, NRF_LOG_MESSAGE_READ_REQUEST));
    CHECK(trace_event_is(&s_fake, 7U, EVENT_READ, 2U, NRF_LOG_MESSAGE_BLOCK));
    CHECK(trace_event_is(&s_fake, 8U, EVENT_WRITE, 2U, NRF_LOG_MESSAGE_READ_REQUEST));
    CHECK(trace_event_is(&s_fake, 9U, EVENT_READ, 2U, NRF_LOG_MESSAGE_BLOCK));
    CHECK(trace_event_is(&s_fake, 10U, EVENT_CLOSE, 2U, 0U));
    CHECK(trace_event_is(&s_fake, 11U, EVENT_CLOSE_VERIFY, 2U, 0U));
    CHECK(s_fake.last_status_id[2] % 32U == 9U);
    fake_stop();

    fake_start(1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.block_writes[0] == 0U);
    CHECK(node_summary(&s_fake, 0U).boot_id_valid);
    fake_stop();

    fake_start(1U);
    fake_append(&s_fake, 0U, 1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    const nrf_log_node_page_t collected = node_page(&s_fake, 0U, false, 0U, 0U, 10U);
    CHECK(collected.count == 1U && collected.records[0].seq == 0U);
    const uint32_t same_boot_id = s_fake.rings[0].boot_id;
    CHECK(fake_init_ring(&s_fake, 0U, same_boot_id) == ESP_OK);
    CHECK(nrf_log_ring_test_set_next_seq(&s_fake.rings[0], 2U) == NRF_LOG_ERROR_OK);
    nrf_log_status_t empty_status = {0};
    CHECK(nrf_log_ring_status_snapshot(&s_fake.rings[0], &empty_status) == NRF_LOG_ERROR_OK);
    CHECK(empty_status.available == 0U);
    CHECK(empty_status.oldest_seq == 2U && empty_status.next_seq == 2U);
    const uint32_t block_writes_before_empty_poll = s_fake.block_writes[0];
    const size_t events_before_empty_poll = s_fake.event_count;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    bool empty_poll_sent_log_read = false;
    for (size_t i = events_before_empty_poll; i < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_WRITE, 0U, NRF_LOG_MESSAGE_READ_REQUEST)) {
            empty_poll_sent_log_read = true;
        }
    }
    CHECK(!empty_poll_sent_log_read);
    CHECK(s_fake.block_writes[0] == block_writes_before_empty_poll);
    fake_stop();

    fake_start(1U);
    fake_append(&s_fake, 0U, 5U);
    s_fake.tx_cost_ms = 30U;
    const nrf_log_collector_iteration_t soft_budget = run_round_at_current_time(&s_fake);
    CHECK(soft_budget.elapsed_ms >= 60U);
    CHECK(s_fake.status_reads[0] == 1U);
    CHECK(s_fake.block_writes[0] == 0U);
    CHECK(soft_budget.nodes_succeeded == 1U);
    CHECK(node_summary(&s_fake, 0U).stored == 0U);
    s_fake.tx_cost_ms = 1U;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.last_block_cursor[0] == 0U);
    CHECK(node_summary(&s_fake, 0U).stored == 5U);
    fake_stop();

    fake_start(5U);
    s_fake.tx_cost_ms = 30U;
    const nrf_log_collector_iteration_t total_budget = run_round_at_current_time(&s_fake);
    CHECK(total_budget.elapsed_ms >= 100U);
    CHECK(total_budget.nodes_attempted < 5U);
    CHECK(total_budget.nodes_attempted <= 2U);
    fake_stop();
}

static void test_framing_faults_and_retries(void)
{
    struct {
        fake_fault_t fault;
        uint8_t response_type;
    } retry_cases[] = {
        { FAULT_SHORT_RX, NRF_LOG_MESSAGE_STATUS },
        { FAULT_BIT_FLIP, NRF_LOG_MESSAGE_BLOCK },
        { FAULT_BIT_BURST, NRF_LOG_MESSAGE_BLOCK },
        { FAULT_STALE_REQUEST_ID, NRF_LOG_MESSAGE_STATUS },
        { FAULT_MALFORMED_SUCCESS, NRF_LOG_MESSAGE_STATUS },
        { FAULT_UNKNOWN_MESSAGE, NRF_LOG_MESSAGE_STATUS },
        { FAULT_NRF_BUSY, NRF_LOG_MESSAGE_BLOCK },
        { FAULT_NACK_AFTER_WRITE, NRF_LOG_MESSAGE_BLOCK },
        { FAULT_TIMEOUT_AFTER_WRITE, NRF_LOG_MESSAGE_BLOCK },
    };
    uint32_t passed = 0U;
    for (size_t i = 0U; i < sizeof(retry_cases) / sizeof(retry_cases[0]); ++i) {
        fake_start(1U);
        fake_append(&s_fake, 0U, 3U);
        s_fake.fault = retry_cases[i].fault;
        s_fake.fault_node = 0U;
        s_fake.fault_response_type = retry_cases[i].response_type;
        CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
        CHECK(node_summary(&s_fake, 0U).stored == 3U);
        CHECK(s_fake.duplicate_deliveries[0] == 1U);
        CHECK(s_fake.block_reads[0] >= 1U);
        if (retry_cases[i].fault == FAULT_NACK_AFTER_WRITE) CHECK(s_fake.recoveries == 0U);
        if (retry_cases[i].fault == FAULT_TIMEOUT_AFTER_WRITE || retry_cases[i].fault == FAULT_NRF_BUSY) {
            CHECK(s_fake.recoveries != 0U);
        }
        ++passed;
        fake_stop();
    }
    CHECK(passed == 9U);

    fake_start(1U);
    fake_append(&s_fake, 0U, 1U);
    s_fake.fault = FAULT_NACK_BEFORE_WRITE;
    s_fake.fault_node = 0U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_STATUS;
    s_fake.fault_always = true;
    CHECK(run_round_at_current_time(&s_fake).nodes_failed == 1U);
    CHECK(s_fake.status_writes[0] == 2U);
    CHECK(s_fake.recoveries == 0U);
    CHECK(node_summary(&s_fake, 0U).stored == 0U);
    s_fake.fault = FAULT_NONE;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.last_block_cursor[0] == 0U);
    CHECK(node_summary(&s_fake, 0U).stored == 1U);
    fake_stop();

    struct {
        fake_fault_t fault;
        uint8_t response_type;
    } corrupt_cases[] = {
        { FAULT_CORRUPT_FOREVER, NRF_LOG_MESSAGE_BLOCK },
        { FAULT_BAD_STATUS_SEMANTICS, NRF_LOG_MESSAGE_STATUS },
        { FAULT_BAD_BLOCK_SEMANTICS, NRF_LOG_MESSAGE_BLOCK },
    };
    for (size_t i = 0U; i < sizeof(corrupt_cases) / sizeof(corrupt_cases[0]); ++i) {
        fake_start(1U);
        fake_append(&s_fake, 0U, 2U);
        s_fake.fault = corrupt_cases[i].fault;
        s_fake.fault_node = 0U;
        s_fake.fault_response_type = corrupt_cases[i].response_type;
        s_fake.fault_always = true;
        CHECK(run_round_at_current_time(&s_fake).nodes_failed == 1U);
        CHECK(s_fake.status_writes[0] == (corrupt_cases[i].response_type == NRF_LOG_MESSAGE_STATUS ? 2U : 1U));
        CHECK(node_summary(&s_fake, 0U).stored == 0U);
        s_fake.last_block_cursor[0] = UINT32_MAX;
        s_fake.fault = FAULT_NONE;
        (void)run_round(&s_fake); /* First round consumes the exponential backoff. */
        CHECK(run_round(&s_fake).nodes_succeeded == 1U);
        CHECK(s_fake.last_block_cursor[0] == 0U);
        CHECK(node_summary(&s_fake, 0U).stored == 2U);
        ++passed;
        fake_stop();
    }

    fake_start(1U);
    fake_append(&s_fake, 0U, 2U);
    s_fake.fault = FAULT_RESET_DURING_RX;
    s_fake.fault_node = 0U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_BLOCK;
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    nrf_log_node_page_t reboot_page = node_page(&s_fake, 0U, false, 0U, 0U, 10U);
    CHECK(reboot_page.count == 1U);
    CHECK(reboot_page.records[0].event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID);
    CHECK(reboot_page.records[0].boot_id == s_fake.rings[0].boot_id);
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    reboot_page = node_page(&s_fake, 0U, false, 0U, 0U, 10U);
    CHECK(reboot_page.count == 2U);
    CHECK(reboot_page.records[1].seq == 0U);
    CHECK(reboot_page.records[1].boot_id == reboot_page.records[0].boot_id);
    fake_stop();

    fake_start(1U);
    fake_append(&s_fake, 0U, 11U);
    s_fake.fault = FAULT_RESET_ON_SECOND_BLOCK;
    s_fake.fault_node = 0U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_BLOCK;
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    CHECK(node_summary(&s_fake, 0U).stored == 6U);
    CHECK(s_fake.last_block_cursor[0] == 5U);
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.last_block_cursor[0] == 0U);
    nrf_log_node_page_t between_reads = node_page(&s_fake, 0U, false, 0U, 0U, 10U);
    CHECK(between_reads.count == 7U);
    CHECK(between_reads.records[4].seq == 4U);
    CHECK(between_reads.records[5].event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID);
    CHECK(between_reads.records[6].seq == 0U);
    CHECK(between_reads.records[6].boot_id == s_fake.rings[0].boot_id);
    fake_stop();
}

static void test_rejected_frames_preserve_collector_state(void)
{
    static const struct {
        fake_fault_t fault;
    } rejection_cases[] = {
        { FAULT_BIT_FLIP },
        { FAULT_BAD_BLOCK_SEMANTICS },
        { FAULT_SHORT_RX },
        { FAULT_UNKNOWN_MESSAGE },
        { FAULT_OTHER_BOOT_ID },
    };

    for (size_t case_index = 0U; case_index < sizeof(rejection_cases) / sizeof(rejection_cases[0]); ++case_index) {
        fake_start(1U);
        fake_append(&s_fake, 0U, 2U);
        CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
        const collector_state_snapshot_t before = snapshot_node(&s_fake, 0U);
        CHECK(before.page.count == 2U);
        fake_append(&s_fake, 0U, 1U);
        s_fake.fault = rejection_cases[case_index].fault;
        s_fake.fault_node = 0U;
        s_fake.fault_response_type = NRF_LOG_MESSAGE_BLOCK;
        s_fake.fault_always = true;
        CHECK(run_round(&s_fake).nodes_failed == 1U);
        const collector_state_snapshot_t after = snapshot_node(&s_fake, 0U);
        check_snapshot_equal(&before, &after);
        CHECK(s_fake.last_block_cursor[0] == before.cursor_seq);

        s_fake.fault = FAULT_NONE;
        s_fake.last_block_cursor[0] = UINT32_MAX;
        CHECK(run_round(&s_fake).nodes_attempted == 0U);
        CHECK(run_round(&s_fake).nodes_succeeded == 1U);
        CHECK(s_fake.last_block_cursor[0] == before.cursor_seq);
        const collector_state_snapshot_t accepted = snapshot_node(&s_fake, 0U);
        CHECK(accepted.page.count == before.page.count + 1U);
        CHECK(accepted.page.records[accepted.page.count - 1U].seq == before.cursor_seq);
        fake_stop();
    }
}

static void test_capability_boot_and_paging(void)
{
    fake_start(1U);
    s_fake.old_firmware[0] = true;
    CHECK(run_round_at_current_time(&s_fake).nodes_failed == 1U);
    CHECK(node_summary(&s_fake, 0U).logs_unsupported);
    const uint32_t old_writes = s_fake.status_writes[0];
    CHECK(run_round(&s_fake).nodes_attempted == 0U);
    CHECK(s_fake.status_writes[0] == old_writes);
    s_fake.targets[0].assignment_generation = 2U;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(!node_summary(&s_fake, 0U).logs_unsupported);
    s_fake.old_firmware[0] = false;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);

    for (uint8_t i = 0U; i < 20U; ++i) {
        fake_append(&s_fake, 0U, 10U);
        (void)run_round(&s_fake);
    }
    nrf_log_node_summary_t summary = node_summary(&s_fake, 0U);
    CHECK(summary.stored == NRF_LOG_COLLECTOR_RECORD_CAPACITY);
    CHECK(summary.local_drop_total != 0U);
    nrf_log_node_page_t first = node_page(&s_fake, 0U, false, 0U, 0U, 10U);
    CHECK(first.count == 10U);
    nrf_log_node_page_t second = node_page(
        &s_fake,
        0U,
        true,
        first.records[9].boot_id,
        first.records[9].seq,
        10U);
    CHECK(second.count == 10U);
    CHECK(second.records[0].seq != first.records[9].seq);
    nrf_log_node_page_t stale = node_page(&s_fake, 0U, true, summary.boot_id, UINT32_C(0xFFFFFFFE), 10U);
    CHECK(stale.reset);
    CHECK(stale.count == 10U);
    CHECK(stale.records[0].seq == first.records[0].seq);

    fake_append(&s_fake, 0U, 140U);
    (void)run_round(&s_fake);
    summary = node_summary(&s_fake, 0U);
    CHECK(summary.wire_gap_total != 0U);
    const uint32_t prior_boot = summary.boot_id;
    const uint32_t wire_gaps_before_reboot = summary.wire_gap_total;
    CHECK(fake_init_ring(&s_fake, 0U, prior_boot + 1U) == ESP_OK);
    fake_append(&s_fake, 0U, 1U);
    (void)run_round(&s_fake);
    summary = node_summary(&s_fake, 0U);
    CHECK(summary.boot_id == prior_boot + 1U);
    CHECK(summary.wire_gap_total == wire_gaps_before_reboot);
    CHECK(summary.wire_gap_total != 0U);
    nrf_log_node_page_t with_marker = node_page(&s_fake, 0U, false, 0U, 0U, 128U);
    bool marker_found = false;
    for (uint16_t i = 0U; i < with_marker.count; ++i) {
        if (with_marker.records[i].event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID) {
            marker_found = true;
            CHECK(with_marker.records[i].arg0 == prior_boot);
            CHECK(with_marker.records[i].arg1 == prior_boot + 1U);
        }
    }
    CHECK(marker_found);

    s_fake.old_firmware[0] = true;
    s_fake.targets[0].assignment_generation = 3U;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(node_summary(&s_fake, 0U).stored == 0U);
    CHECK(!node_summary(&s_fake, 0U).logs_unsupported);
    fake_stop();
}

static void test_round_cadence_and_yield(void)
{
    fake_start(1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    const uint32_t initial_status_writes = s_fake.status_writes[0];
    const uint64_t first_round_ms = 100U;
    s_fake.now_ms = first_round_ms + ROUND_MS - 1U;
    CHECK(run_round_at_current_time(&s_fake).nodes_attempted == 0U);
    CHECK(s_fake.status_writes[0] == initial_status_writes);
    s_fake.now_ms = first_round_ms + ROUND_MS;
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.status_writes[0] == initial_status_writes + 1U);

    s_fake.yield_after_status_read = true;
    fake_append(&s_fake, 0U, 2U);
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.block_writes[0] == 0U);
    CHECK(s_fake.yield_pending);
    CHECK(s_fake.events[s_fake.event_count - 1U].kind != EVENT_WRITE ||
          s_fake.events[s_fake.event_count - 1U].message_type != NRF_LOG_MESSAGE_READ_REQUEST);
    fake_stop();
}

static void test_log_collector_round_skipped_when_bus_busy(void)
{
    fake_start(2U);
    s_fake.begin_node_error = SENSOR_BUS_ERR_MUTEX_BUSY;

    const nrf_log_collector_iteration_t skipped = run_round_at_current_time(&s_fake);
    CHECK(skipped.nodes_attempted == 0U);
    CHECK(skipped.nodes_failed == 0U);

    const nrf_log_collector_iteration_t retried = run_round(&s_fake);
    CHECK(retried.nodes_attempted > 0U);
    CHECK(retried.nodes_failed == 0U);
    CHECK(retried.nodes_succeeded == retried.nodes_attempted);
    fake_stop();
}

static void test_five_node_tca_matrix(void)
{
    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    nrf_log_collector_iteration_t result = run_round_at_current_time(&s_fake);
    CHECK(result.nodes_attempted == 5U);
    for (uint8_t node = 0U; node < 5U; ++node) assert_record_count(&s_fake, node, 1U);
    CHECK(s_fake.last_status_id[0] != s_fake.last_status_id[1]);
    CHECK(s_fake.last_status_id[1] != s_fake.last_status_id[2]);
    const size_t second_round_start = s_fake.event_count;
    result = run_round(&s_fake);
    CHECK(result.nodes_attempted == 5U);
    uint8_t first_second_round_node = UINT8_MAX;
    for (size_t i = second_round_start; i < s_fake.event_count; ++i) {
        if (s_fake.events[i].kind == EVENT_WRITE) {
            first_second_round_node = s_fake.events[i].node;
            break;
        }
    }
    CHECK(first_second_round_node == 1U);
    fake_stop();

    for (uint8_t absent = 1U; absent <= 4U; ++absent) {
        fake_start(5U);
        s_fake.targets[absent].present = false;
        CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
        for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
        result = run_round_at_current_time(&s_fake);
        CHECK(result.nodes_attempted == 4U);
        CHECK(s_fake.status_writes[absent] == 0U);
        assert_record_count(&s_fake, absent, 0U);
        for (uint8_t node = 0U; node < 5U; ++node) {
            if (node != absent) assert_record_count(&s_fake, node, 1U);
        }
        fake_stop();
    }

    const uint8_t absent_counts[] = {2U, 3U, 4U};
    for (size_t scenario = 0U; scenario < sizeof(absent_counts); ++scenario) {
        fake_start(5U);
        for (uint8_t node = 1U; node <= absent_counts[scenario]; ++node) {
            s_fake.targets[node].present = false;
        }
        CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
        for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
        result = run_round_at_current_time(&s_fake);
        CHECK(result.nodes_attempted == 5U - absent_counts[scenario]);
        for (uint8_t node = 1U; node <= absent_counts[scenario]; ++node) {
            CHECK(s_fake.status_writes[node] == 0U);
            assert_record_count(&s_fake, node, 0U);
        }
        fake_stop();
    }

    fake_start(5U);
    s_fake.targets[0].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    result = run_round_at_current_time(&s_fake);
    CHECK(result.nodes_attempted == 4U);
    CHECK(s_fake.status_writes[0] == 0U);
    assert_record_count(&s_fake, 0U, 0U);
    for (uint8_t node = 1U; node < 5U; ++node) assert_record_count(&s_fake, node, 1U);
    fake_stop();

    fake_start(5U);
    fake_append(&s_fake, 1U, 100U);
    for (uint8_t node = 0U; node < 5U; ++node) if (node != 1U) fake_append(&s_fake, node, 1U);
    result = run_round_at_current_time(&s_fake);
    CHECK(result.elapsed_ms < 100U);
    CHECK(result.nodes_attempted == 5U);
    CHECK(s_fake.block_writes[1] == 2U);
    CHECK(node_summary(&s_fake, 1U).stored == 10U);
    for (uint8_t node = 0U; node < 5U; ++node) {
        if (node != 1U) assert_record_count(&s_fake, node, 1U);
    }
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    s_fake.fault = FAULT_CORRUPT_FOREVER;
    s_fake.fault_node = 1U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_BLOCK;
    s_fake.fault_always = true;
    s_fake.other_channel = 2U;
    result = run_round_at_current_time(&s_fake);
    CHECK(result.nodes_attempted == 5U);
    CHECK(node_summary(&s_fake, 1U).stored == 0U);
    for (uint8_t node = 0U; node < 5U; ++node) {
        if (node != 1U) assert_record_count(&s_fake, node, 1U);
    }
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    s_fake.select_failure = true;
    s_fake.select_failure_node = 2U;
    result = run_round_at_current_time(&s_fake);
    CHECK(result.nodes_attempted == 5U);
    bool select_closed_in_order = false;
    for (size_t i = 0U; i + 1U < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_SELECT, 2U, 0U) &&
            trace_event_is(&s_fake, i + 1U, EVENT_CLOSE, 2U, 0U)) {
            select_closed_in_order = true;
        }
    }
    CHECK(select_closed_in_order);
    assert_record_count(&s_fake, 2U, 0U);
    assert_record_count(&s_fake, 3U, 1U);
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    s_fake.readback_mismatch = true;
    s_fake.select_failure_node = 3U;
    (void)run_round_at_current_time(&s_fake);
    bool readback_recovered = false;
    for (size_t i = 0U; i + 3U < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_SELECT, 3U, 0U) &&
            trace_event_is(&s_fake, i + 1U, EVENT_SELECT_VERIFY, 3U, 0U) &&
            trace_event_is(&s_fake, i + 2U, EVENT_CLOSE, 3U, 0U) &&
            trace_event_is(&s_fake, i + 3U, EVENT_CLOSE_VERIFY, 3U, 0U)) {
            readback_recovered = true;
        }
    }
    CHECK(readback_recovered);
    assert_record_count(&s_fake, 3U, 0U);
    assert_record_count(&s_fake, 4U, 1U);
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    s_fake.close_failure = true;
    s_fake.close_failure_node = 2U;
    (void)run_round_at_current_time(&s_fake);
    bool close_recovery_ordered = false;
    for (size_t i = 0U; i + 3U < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_CLOSE, 2U, 0U) &&
            trace_event_is(&s_fake, i + 1U, EVENT_CLOSE_VERIFY, 2U, 0U) &&
            trace_event_is(&s_fake, i + 2U, EVENT_CLOSE, 2U, 0U) &&
            trace_event_is(&s_fake, i + 3U, EVENT_CLOSE_VERIFY, 2U, 0U)) {
            close_recovery_ordered = true;
        }
    }
    CHECK(close_recovery_ordered);
    assert_record_count(&s_fake, 2U, 0U);
    assert_record_count(&s_fake, 3U, 1U);
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 5U);
    const uint32_t node_two_blocks = s_fake.block_writes[2];
    s_fake.fault = FAULT_CHANNEL_CHANGED;
    s_fake.fault_node = 2U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_STATUS;
    s_fake.fault_always = true;
    s_fake.other_channel = 1U;
    result = run_round(&s_fake);
    CHECK(result.nodes_failed == 1U);
    CHECK(s_fake.block_writes[2] == node_two_blocks);
    assert_record_count(&s_fake, 2U, 1U);
    bool saw_channel_change = false;
    for (size_t i = 0U; i < s_fake.event_count; ++i) {
        if (s_fake.events[i].kind == EVENT_CHANNEL_CHANGE) saw_channel_change = true;
    }
    CHECK(saw_channel_change);
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 5U);
    for (uint8_t round = 0U; round < 3U; ++round) {
        const uint32_t next_boot = s_fake.rings[1].boot_id + 1U;
        CHECK(fake_init_ring(&s_fake, 1U, next_boot) == ESP_OK);
        fake_append(&s_fake, 1U, 1U);
        (void)run_round(&s_fake);
    }
    nrf_log_node_page_t repeated_reboots = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
    uint8_t reboot_markers = 0U;
    for (uint16_t i = 0U; i < repeated_reboots.count; ++i) {
        if (repeated_reboots.records[i].event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID) ++reboot_markers;
    }
    CHECK(reboot_markers == 3U);
    fake_stop();

    fake_start(2U);
    fake_append(&s_fake, 1U, 1U);
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 2U);
    s_fake.targets[1].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    s_fake.targets[1].present = true;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(node_summary(&s_fake, 1U).stored == 1U);
    s_fake.targets[1].assignment_generation++;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(node_summary(&s_fake, 1U).stored == 0U);
    s_fake.targets[1].assignment_generation++;
    s_fake.targets[1].hardware_id[0]++;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    CHECK(node_summary(&s_fake, 1U).stored == 0U);
    fake_stop();
}

static void test_backoff_storms_and_random_run(void)
{
    fake_start(1U);
    fake_append(&s_fake, 0U, 1U);
    s_fake.targets[0].cs_busy = true;
    const nrf_log_collector_iteration_t cs_busy = run_round(&s_fake);
    CHECK(cs_busy.nodes_attempted == 0U);
    CHECK(cs_busy.nodes_skipped_cs == 1U);
    CHECK(s_fake.event_count == 0U);
    fake_stop();

    fake_start(2U);
    s_fake.fault = FAULT_TIMEOUT_BEFORE_WRITE;
    s_fake.fault_node = 0U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_STATUS;
    s_fake.fault_always = true;
    for (uint8_t round = 0U; round < 60U; ++round) {
        const nrf_log_collector_iteration_t result = run_round(&s_fake);
        CHECK(result.nodes_attempted >= 1U && result.nodes_attempted <= 2U);
        CHECK(s_fake.status_writes[1] == (uint32_t)round + 1U);
    }
    CHECK(s_fake.status_writes[0] == 12U);
    CHECK(s_fake.recoveries == 12U);
    CHECK(s_fake.status_writes[0] + s_fake.recoveries < 40U);
    fake_stop();

    const uint32_t rates[] = {1U, 10U, 100U};
    for (uint8_t node_count_choice = 0U; node_count_choice < 2U; ++node_count_choice) {
        const uint8_t node_count = node_count_choice == 0U ? 1U : 5U;
        for (size_t rate_index = 0U; rate_index < sizeof(rates) / sizeof(rates[0]); ++rate_index) {
            fake_start(node_count);
            for (uint8_t round = 0U; round < 25U; ++round) {
                for (uint8_t node = 0U; node < node_count; ++node) fake_append(&s_fake, node, rates[rate_index]);
                const nrf_log_collector_iteration_t result = run_round(&s_fake);
                CHECK(result.nodes_attempted == node_count);
                CHECK(result.elapsed_ms <= 100U);
                for (uint8_t node = 0U; node < node_count; ++node) {
                    CHECK(s_fake.status_writes[node] == (uint32_t)round + 1U);
                    CHECK(node_summary(&s_fake, node).stored <= NRF_LOG_COLLECTOR_RECORD_CAPACITY);
                }
            }
            if (rates[rate_index] == 100U) {
                for (uint8_t node = 0U; node < node_count; ++node) {
                    const nrf_log_node_summary_t summary = node_summary(&s_fake, node);
                    CHECK(summary.local_drop_total != 0U);
                    CHECK(summary.overwritten_total != 0U);
                }
            }
            fake_stop();
        }
    }

    fake_start(5U);
    for (uint8_t round = 0U; round < 60U; ++round) {
        if (round != 0U) s_fake.now_ms += ROUND_MS;
        for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 100U);
        s_fake.deadlines_enabled = true;
        s_fake.cs_deadline_ms = s_fake.now_ms + 2U;
        s_fake.sensor_deadline_ms = s_fake.now_ms + 2U;
        s_fake.reconcile_deadline_ms = s_fake.now_ms + 2U;
        const nrf_log_collector_iteration_t result = run_round_at_current_time(&s_fake);
        CHECK(result.elapsed_ms <= 100U);
        fake_service_due_deadlines(&s_fake);
        CHECK(s_fake.cs_deadlines_served == (uint32_t)round + 1U);
        CHECK(s_fake.sensor_deadlines_served == (uint32_t)round + 1U);
        CHECK(s_fake.reconcile_deadlines_served == (uint32_t)round + 1U);
    }
    CHECK(s_fake.cs_deadline_max_lateness_ms <= 4U);
    CHECK(s_fake.sensor_deadline_max_lateness_ms <= 4U);
    CHECK(s_fake.reconcile_deadline_max_lateness_ms <= 4U);
    uint32_t min_deadline_storm_polls = UINT32_MAX;
    uint32_t max_deadline_storm_polls = 0U;
    for (uint8_t node = 0U; node < 5U; ++node) {
        if (s_fake.status_writes[node] < min_deadline_storm_polls) min_deadline_storm_polls = s_fake.status_writes[node];
        if (s_fake.status_writes[node] > max_deadline_storm_polls) max_deadline_storm_polls = s_fake.status_writes[node];
    }
    CHECK(min_deadline_storm_polls != 0U);
    CHECK(max_deadline_storm_polls - min_deadline_storm_polls <= 1U);
    fake_stop();

    fake_start(5U);
    for (uint8_t node = 0U; node < 5U; ++node) fake_append(&s_fake, node, 100U);
    s_fake.yield_after_status_read = true; /* Pending sensor/reconcile/CS work at a transaction boundary. */
    const nrf_log_collector_iteration_t yielded = run_round_at_current_time(&s_fake);
    CHECK(yielded.nodes_attempted == 1U);
    CHECK(s_fake.block_writes[0] == 0U);
    CHECK(s_fake.status_reads[0] == 1U);
    CHECK(s_fake.yield_pending);
    fake_stop();

    const uint32_t seed = UINT32_C(0x308E5A71);
    uint32_t random = seed;
    active_seed = seed;
    fake_start(5U);
    printf("MAX-308 seeded collector run seed=0x%08X\n", seed);
    for (uint16_t round = 0U; round < 250U; ++round) {
        for (uint8_t node = 0U; node < 5U; ++node) {
            random ^= random << 13U;
            random ^= random >> 17U;
            random ^= random << 5U;
            fake_append(&s_fake, node, random % 7U);
            s_fake.targets[node].cs_busy = (random & 0x100U) != 0U;
        }
        const nrf_log_collector_iteration_t result = run_round(&s_fake);
        CHECK(result.elapsed_ms <= 100U);
        for (uint8_t node = 0U; node < 5U; ++node) {
            nrf_log_node_summary_t summary = node_summary(&s_fake, node);
            CHECK(summary.stored <= NRF_LOG_COLLECTOR_RECORD_CAPACITY);
            nrf_log_node_page_t page = node_page(&s_fake, node, false, 0U, 0U, 128U);
            for (uint16_t i = 1U; i < page.count; ++i) {
                if (page.records[i - 1U].boot_id == page.records[i].boot_id &&
                    page.records[i - 1U].event_id != NRF_LOG_COLLECTOR_REBOOT_EVENT_ID &&
                    page.records[i].event_id != NRF_LOG_COLLECTOR_REBOOT_EVENT_ID) {
                    CHECK(page.records[i].seq > page.records[i - 1U].seq);
                }
            }
        }
    }
    fake_stop();
    active_seed = 0U;
}

static void test_store_interleavings(void)
{
    fake_start(1U);
    fake_append(&s_fake, 0U, 2U);
    s_fake.inject_page_read_during_write = true;
    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    CHECK(node_summary(&s_fake, 0U).stored == 2U);
    fake_stop();

    fake_start(2U);
    fake_append(&s_fake, 1U, 5U);
    s_fake.yield_after_status_read = true; /* Worker stop/refresh signal arrives mid-poll. */
    s_fake.yield_after_status_node = 1U;
    const nrf_log_collector_iteration_t stopped = run_round_at_current_time(&s_fake);
    CHECK(stopped.nodes_succeeded == 2U);
    CHECK(s_fake.block_writes[1] == 0U);
    CHECK(s_fake.events[s_fake.event_count - 1U].kind == EVENT_CLOSE_VERIFY);
    fake_stop();

    fake_start(1U);
    fake_append(&s_fake, 0U, 2U);
    s_fake.fault = FAULT_TIMEOUT_AFTER_WRITE;
    s_fake.fault_node = 0U;
    s_fake.fault_response_type = NRF_LOG_MESSAGE_STATUS;
    const nrf_log_collector_iteration_t recovered = run_round_at_current_time(&s_fake);
    CHECK(recovered.nodes_succeeded == 1U);
    CHECK(s_fake.recoveries == 1U);
    CHECK(s_fake.events[0U].kind == EVENT_WRITE);
    CHECK(s_fake.events[1U].kind == EVENT_RECOVERY);
    CHECK(s_fake.events[2U].kind == EVENT_WRITE);
    CHECK(node_summary(&s_fake, 0U).stored == 2U);
    fake_stop();
}

static void test_full_ring_reboot_interleavings(void)
{
    static const struct {
        reset_injection_point_t point;
        uint16_t old_records_kept;
        uint8_t expected_block_reads;
    } cases[] = {
        { RESET_AFTER_STATUS_READ, 10U, 1U },
        { RESET_AFTER_FIRST_BLOCK_READ, 15U, 2U },
    };

    for (size_t case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        fake_start(2U);
        s_fake.targets[0].present = false;
        CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
        fake_append(&s_fake, 1U, NRF_LOG_RING_CAPACITY);
        CHECK(s_fake.rings[1].count == NRF_LOG_RING_CAPACITY);
        CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
        const uint32_t old_boot_id = s_fake.rings[1].boot_id;
        nrf_log_node_page_t page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
        CHECK(page.count == 10U);
        CHECK(node_summary(&s_fake, 1U).boot_id == old_boot_id);
        for (uint16_t i = 0U; i < page.count; ++i) {
            CHECK(page.records[i].boot_id == old_boot_id);
            CHECK(page.records[i].seq == i);
        }

        s_fake.event_count = 0U;
        const uint32_t prior_block_reads = s_fake.block_reads[1];
        s_fake.reset_injection_point = cases[case_index].point;
        s_fake.reset_injection_node = 1U;
        s_fake.reset_block_read_target = prior_block_reads + 1U;
        const nrf_log_collector_iteration_t reset_iteration = run_round(&s_fake);
        CHECK(reset_iteration.nodes_attempted == 1U);
        CHECK(reset_iteration.nodes_succeeded == 1U);
        CHECK(s_fake.node_reset_injected);
        CHECK(s_fake.reset_old_boot_id == old_boot_id);
        CHECK(s_fake.reset_new_boot_id == old_boot_id + 1U);
        CHECK(s_fake.rings[1].count == 1U);
        CHECK(s_fake.rings[1].boot_id == s_fake.reset_new_boot_id);
        CHECK(s_fake.block_reads[1] - prior_block_reads == cases[case_index].expected_block_reads);
        CHECK(!s_fake.mux_open);
        CHECK(s_fake.mux_selects == s_fake.mux_closes);

        page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
        CHECK(page.count == (uint16_t)(cases[case_index].old_records_kept + 1U));
        for (uint16_t i = 0U; i < cases[case_index].old_records_kept; ++i) {
            CHECK(page.records[i].boot_id == old_boot_id);
            CHECK(page.records[i].seq == i);
        }
        const nrf_log_stored_record_t *marker = &page.records[cases[case_index].old_records_kept];
        CHECK(marker->event_id == NRF_LOG_COLLECTOR_REBOOT_EVENT_ID);
        CHECK(marker->boot_id == s_fake.reset_new_boot_id);
        CHECK(marker->seq == UINT32_MAX);
        CHECK(marker->arg0 == old_boot_id);
        CHECK(marker->arg1 == s_fake.reset_new_boot_id);
        nrf_log_node_summary_t summary = node_summary(&s_fake, 1U);
        CHECK(summary.boot_id_valid && summary.boot_id == s_fake.reset_new_boot_id);
        CHECK(summary.overwritten_total == 0U);
        CHECK(summary.wire_gap_total == 0U);
        CHECK(summary.local_drop_total == 0U);

        size_t reset_event = s_fake.event_count;
        for (size_t i = 0U; i < s_fake.event_count; ++i) {
            if (trace_event_is(&s_fake, i, EVENT_NODE_RESET, 1U,
                    cases[case_index].point == RESET_AFTER_STATUS_READ
                        ? NRF_LOG_MESSAGE_STATUS
                        : NRF_LOG_MESSAGE_BLOCK)) {
                reset_event = i;
                break;
            }
        }
        CHECK(reset_event > 0U && reset_event < s_fake.event_count);
        CHECK(s_fake.events[reset_event - 1U].kind == EVENT_READ);
        CHECK(s_fake.events[reset_event - 1U].message_type ==
            (cases[case_index].point == RESET_AFTER_STATUS_READ
                ? NRF_LOG_MESSAGE_STATUS
                : NRF_LOG_MESSAGE_BLOCK));
        CHECK(trace_event_is(&s_fake, reset_event + 1U, EVENT_WRITE, 1U,
            NRF_LOG_MESSAGE_READ_REQUEST));

        CHECK(run_round(&s_fake).nodes_succeeded == 1U);
        page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
        CHECK(page.count == (uint16_t)(cases[case_index].old_records_kept + 2U));
        CHECK(s_fake.last_block_cursor[1] == 0U);
        CHECK(page.records[page.count - 1U].boot_id == s_fake.reset_new_boot_id);
        CHECK(page.records[page.count - 1U].seq == 0U);
        summary = node_summary(&s_fake, 1U);
        CHECK(summary.boot_id == s_fake.reset_new_boot_id);
        CHECK(summary.wire_gap_total == 0U);
        CHECK(!s_fake.mux_open);
        CHECK(s_fake.mux_selects == s_fake.mux_closes);
        fake_stop();
    }
}

static void test_reconcile_during_bus_transaction(void)
{
    fake_start(3U);
    s_fake.targets[0].present = false;
    s_fake.targets[2].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    fake_append(&s_fake, 1U, 10U);
    fake_append(&s_fake, 2U, 2U);
    const uint32_t old_boot_id = s_fake.rings[1].boot_id;
    const uint32_t new_boot_id = s_fake.rings[2].boot_id;
    const uint8_t old_mux_channel = s_fake.targets[1].mux_channel;
    s_fake.reconcile_on_first_block_write = true;
    s_fake.reconcile_node = 1U;
    s_fake.reconcile_new_physical_node = 2U;
    s_fake.reconcile_new_mux_channel = (uint8_t)(NRF_TOPOLOGY_REMOTE_COUNT - 1U);

    CHECK(run_round_at_current_time(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.reconcile_injected);
    CHECK(s_fake.targets[1].node_id == (uint8_t)(UINT8_C(0x40) + 1U));
    CHECK(s_fake.targets[1].assignment_generation == 2U);
    CHECK(s_fake.route_physical_node[1] == 2U);
    CHECK(s_fake.rings[1].boot_id == old_boot_id);
    nrf_log_node_page_t page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);

    size_t reconcile_event = s_fake.event_count;
    for (size_t i = 0U; i < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_RECONCILE, 1U, NRF_LOG_MESSAGE_READ_REQUEST)) {
            reconcile_event = i;
            break;
        }
    }
    CHECK(reconcile_event > 0U && reconcile_event + 1U < s_fake.event_count);
    CHECK(trace_event_is(&s_fake, reconcile_event - 1U, EVENT_WRITE, 1U,
        NRF_LOG_MESSAGE_READ_REQUEST));
    CHECK(s_fake.events[reconcile_event].physical_node == 1U);
    CHECK(s_fake.events[reconcile_event].mux_channel == old_mux_channel);
    CHECK(trace_event_is(&s_fake, reconcile_event + 1U, EVENT_READ, 1U, NRF_LOG_MESSAGE_BLOCK));
    CHECK(s_fake.events[reconcile_event + 1U].physical_node == 1U);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == s_fake.mux_closes);

    bool stored_old_assignment = false;
    for (uint16_t i = 0U; i < page.count; ++i) {
        if (page.records[i].event_id != NRF_LOG_COLLECTOR_REBOOT_EVENT_ID &&
            page.records[i].arg0 == 1U) {
            stored_old_assignment = true;
        }
    }
    /* The in-flight stage was captured under A, while sync_targets replaced slot 1 with B. */
    CHECK_PRODUCTION_INVARIANT(!stored_old_assignment,
        "MAX-308 10.4: staged node A records were committed into node B's store after in-transaction reconcile");

    const size_t new_mapping_start = s_fake.event_count;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    CHECK(s_fake.last_block_cursor[1] == 0U);
    size_t new_mapping_select = s_fake.event_count;
    for (size_t i = new_mapping_start; i < s_fake.event_count; ++i) {
        if (s_fake.events[i].kind == EVENT_SELECT && s_fake.events[i].node == 1U) {
            new_mapping_select = i;
            break;
        }
    }
    CHECK(new_mapping_select < s_fake.event_count);
    CHECK(s_fake.events[new_mapping_select].physical_node == 2U);
    CHECK(s_fake.events[new_mapping_select].mux_channel == s_fake.reconcile_new_mux_channel);
    page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
    CHECK(node_summary(&s_fake, 1U).boot_id == new_boot_id);
    CHECK(page.count >= 2U);
    CHECK(page.records[page.count - 2U].boot_id == new_boot_id);
    CHECK(page.records[page.count - 2U].seq == 0U);
    CHECK(page.records[page.count - 2U].arg0 == 2U);
    CHECK(page.records[page.count - 1U].boot_id == new_boot_id);
    CHECK(page.records[page.count - 1U].seq == 1U);
    CHECK(page.records[page.count - 1U].arg0 == 2U);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == s_fake.mux_closes);
    fake_stop();
}

static void test_shutdown_during_bus_transaction(void)
{
    fake_start(2U);
    s_fake.targets[0].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    fake_append(&s_fake, 1U, 11U);
    const uint32_t boot_id = s_fake.rings[1].boot_id;
    s_fake.shutdown_after_first_block_read = true;

    const nrf_log_collector_iteration_t stopped = run_round_at_current_time(&s_fake);
    CHECK(stopped.nodes_attempted == 1U);
    CHECK(stopped.nodes_succeeded == 1U);
    CHECK(s_fake.shutdown_injected && s_fake.shutdown_requested);
    CHECK(s_fake.block_writes[1] == 1U);
    CHECK(s_fake.block_reads[1] == 1U);
    CHECK(s_fake.last_block_cursor[1] == 0U);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == 1U && s_fake.mux_closes == 1U);

    size_t shutdown_event = s_fake.event_count;
    for (size_t i = 0U; i < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_SHUTDOWN, 1U, NRF_LOG_MESSAGE_BLOCK)) {
            shutdown_event = i;
            break;
        }
    }
    CHECK(shutdown_event > 0U && shutdown_event + 3U == s_fake.event_count);
    CHECK(s_fake.events[shutdown_event - 1U].kind == EVENT_READ);
    CHECK(s_fake.events[shutdown_event + 1U].kind == EVENT_CLOSE);
    CHECK(s_fake.events[shutdown_event + 2U].kind == EVENT_CLOSE_VERIFY);
    for (size_t i = shutdown_event + 1U; i < s_fake.event_count; ++i) {
        CHECK(s_fake.events[i].kind != EVENT_WRITE && s_fake.events[i].kind != EVENT_READ);
    }

    nrf_log_node_summary_t summary = node_summary(&s_fake, 1U);
    CHECK(summary.boot_id_valid && summary.boot_id == boot_id);
    CHECK(summary.stored == NRF_LOG_PROTOCOL_MAX_RECORDS);
    CHECK(summary.overwritten_total == 0U);
    CHECK(summary.wire_gap_total == 0U);
    CHECK(summary.local_drop_total == 0U);
    nrf_log_node_page_t page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
    CHECK(page.count == NRF_LOG_PROTOCOL_MAX_RECORDS);
    for (uint16_t i = 0U; i < page.count; ++i) {
        CHECK(page.records[i].boot_id == boot_id);
        CHECK(page.records[i].seq == i);
    }

    s_fake.shutdown_requested = false;
    s_fake.shutdown_after_first_block_read = false;
    CHECK(run_round(&s_fake).nodes_succeeded == 1U);
    /* poll_node reads up to two blocks per round, so the resumed round
     * drains the remaining six records: blocks at cursor 5 and 10. */
    CHECK(s_fake.last_block_cursor[1] == 2U * NRF_LOG_PROTOCOL_MAX_RECORDS);
    CHECK(node_summary(&s_fake, 1U).stored == 11U);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == s_fake.mux_closes);
    fake_stop();
}

static void test_priority_storm_during_refresh_and_cs(void)
{
    fake_start(5U);
    s_fake.targets[0].present = false;
    CHECK(nrf_log_collector_sync_targets(s_fake.collector, s_fake.targets) == ESP_OK);
    fake_append(&s_fake, 1U, NRF_LOG_RING_CAPACITY);
    s_fake.priority_storm_after_status_read = true;
    s_fake.priority_storm_node = 1U;

    const nrf_log_collector_iteration_t yielded = run_round_at_current_time(&s_fake);
    CHECK(yielded.nodes_attempted == 1U);
    CHECK(yielded.nodes_succeeded == 1U);
    CHECK(s_fake.priority_storm_injected);
    CHECK(s_fake.sensor_refresh_active && s_fake.cs_command_active);
    CHECK(s_fake.targets[1].cs_busy && s_fake.targets[2].cs_busy);
    CHECK(s_fake.block_writes[1] == 0U);
    CHECK(s_fake.status_reads[1] == 1U);
    CHECK(s_fake.cs_deadlines_served == 1U);
    CHECK(s_fake.sensor_deadlines_served == 1U);
    CHECK(s_fake.reconcile_deadlines_served == 1U);
    CHECK(s_fake.cs_deadline_max_lateness_ms == 0U);
    CHECK(s_fake.sensor_deadline_max_lateness_ms == 0U);
    CHECK(s_fake.reconcile_deadline_max_lateness_ms == 0U);
    CHECK(!s_fake.deadlines_enabled);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == 1U && s_fake.mux_closes == 1U);

    nrf_log_status_t ring_status = {0};
    CHECK(nrf_log_ring_status_snapshot(&s_fake.rings[1], &ring_status) == NRF_LOG_ERROR_OK);
    CHECK(ring_status.overwritten_total == 16U);
    CHECK(ring_status.oldest_seq == 16U);
    nrf_log_node_summary_t summary = node_summary(&s_fake, 1U);
    CHECK(summary.boot_id_valid && summary.boot_id == s_fake.rings[1].boot_id);
    CHECK(summary.stored == 0U);
    CHECK(summary.overwritten_total == 0U);

    size_t storm_event = s_fake.event_count;
    for (size_t i = 0U; i < s_fake.event_count; ++i) {
        if (trace_event_is(&s_fake, i, EVENT_PRIORITY_STORM, 1U, NRF_LOG_MESSAGE_STATUS)) {
            storm_event = i;
            break;
        }
    }
    CHECK(storm_event > 0U && storm_event + 3U == s_fake.event_count);
    CHECK(s_fake.events[storm_event - 1U].kind == EVENT_READ);
    CHECK(s_fake.events[storm_event + 1U].kind == EVENT_CLOSE);
    CHECK(s_fake.events[storm_event + 2U].kind == EVENT_CLOSE_VERIFY);
    for (size_t i = storm_event + 1U; i < s_fake.event_count; ++i) {
        CHECK(s_fake.events[i].kind != EVENT_WRITE && s_fake.events[i].kind != EVENT_READ);
    }

    s_fake.sensor_refresh_active = false;
    s_fake.cs_command_active = false;
    s_fake.targets[1].cs_busy = false;
    s_fake.targets[2].cs_busy = false;
    const nrf_log_collector_iteration_t resumed = run_round(&s_fake);
    CHECK(resumed.nodes_attempted == 4U);
    uint32_t min_status_writes = UINT32_MAX;
    uint32_t max_status_writes = 0U;
    for (uint8_t node = 1U; node <= 4U; ++node) {
        CHECK(s_fake.status_writes[node] != 0U);
        CHECK(node_summary(&s_fake, node).stored == (node == 1U ? 10U : 2U));
        if (s_fake.status_writes[node] < min_status_writes) min_status_writes = s_fake.status_writes[node];
        if (s_fake.status_writes[node] > max_status_writes) max_status_writes = s_fake.status_writes[node];
    }
    CHECK(max_status_writes - min_status_writes <= 1U);
    summary = node_summary(&s_fake, 1U);
    CHECK(summary.overwritten_total == 16U);
    CHECK(summary.wire_gap_total == 1U);
    CHECK(summary.lost_internal_total == 0U);
    CHECK(summary.local_drop_total == 0U);
    nrf_log_node_page_t page = node_page(&s_fake, 1U, false, 0U, 0U, 128U);
    CHECK(page.count == 10U);
    CHECK(s_fake.last_block_cursor[1] == 21U);
    CHECK(page.records[0].seq == 16U);
    CHECK(page.records[9].seq == 25U);
    CHECK(!s_fake.mux_open);
    CHECK(s_fake.mux_selects == s_fake.mux_closes);
    fake_stop();
}

int main(void)
{
    volatile uint32_t collector_period_ms = NRF_LOG_COLLECTOR_PERIOD_MS;
    CHECK(collector_period_ms == 1000U);
    CHECK(collector_period_ms == ROUND_MS);
    const struct {
        const char *name;
        void (*run)(void);
    } sections[] = {
        { "section 4 framing/budget/order", test_framing_budget_and_order },
        { "sections 5-7 corruption/fault/reboot", test_framing_faults_and_retries },
        { "section 12.3 rejected frames preserve state", test_rejected_frames_preserve_collector_state },
        { "capability/generation/paging/store", test_capability_boot_and_paging },
        { "round cadence/pending-work yield", test_round_cadence_and_yield },
        { "busy bus skips collector round without failure", test_log_collector_round_skipped_when_bus_busy },
        { "section 8 five-node TCA matrix", test_five_node_tca_matrix },
        { "section 9 fairness/storm/deadlines/seeded", test_backoff_storms_and_random_run },
        { "section 10.3 full-ring reset interleavings", test_full_ring_reboot_interleavings },
        { "section 10.4 reconcile inside bus transaction", test_reconcile_during_bus_transaction },
        { "section 10.5 stop inside bus transaction", test_shutdown_during_bus_transaction },
        { "section 10.7 refresh/CS priority storm", test_priority_storm_during_refresh_and_cs },
        { "section 10 concurrency/shutdown/recovery", test_store_interleavings },
    };
    for (size_t i = 0U; i < sizeof(sections) / sizeof(sections[0]); ++i) {
        const uint32_t before = s_assertions;
        const uint32_t defects_before = s_production_defects;
        sections[i].run();
        printf("%s: %s checks=%u production_defects=%u\n",
            sections[i].name,
            s_production_defects == defects_before ? "PASS" : "FAIL",
            s_assertions - before,
            s_production_defects - defects_before);
    }
    printf("nRF log collector host assertions: %u\n", s_assertions);
    return s_production_defects == 0U ? 0 : 1;
}
