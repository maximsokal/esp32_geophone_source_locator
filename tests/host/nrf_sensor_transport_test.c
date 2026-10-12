#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_sensor_transport.h"
#include "nrf_node_link_status_policy.h"

typedef enum {
    M_OK,
    M_FOREIGN,
    M_FOREIGN_TWICE,
    M_PENDING,
    M_NACK,
    M_PROTOCOL,
    M_PARTIAL,
    M_NO_DATA,
    M_BUSY,
    M_ABORT_AFTER_PENDING,
    M_SNAPSHOT_IDENTITY_MISMATCH,
    M_STATUS_BOOTING,
} mode_t;

typedef enum {
    E_SELECT,
    E_MAIN_READ,
    E_REMOTE_READ,
    E_WRITE,
    E_SNAPSHOT,
    E_CLOSE,
} event_t;

typedef struct {
    int sel;
    uint8_t ch;
    uint64_t now;
    uint32_t req;
    uint32_t remote_epoch;
    uint8_t remote_hw;
    uint32_t snapshot_sequence;
    uint64_t source_uptime_ms;
    mode_t mode;
    event_t events[128];
    unsigned event_count;
    unsigned select;
    unsigned close;
    unsigned main_reads;
    unsigned remote_reads;
    unsigned snaps;
    unsigned writes;
    unsigned discovery;
    bool main_absent;
    bool main_bad_epoch;
    bool fail_select;
    bool fail_close;
    bool mux_fault;
    bool abort;
} bus_t;

static void record_event(bus_t *bus, event_t event)
{
    assert(bus->event_count < sizeof(bus->events) / sizeof(bus->events[0]));
    bus->events[bus->event_count++] = event;
}

static void node(
    nrf_topology_node_snapshot_t *node_snapshot,
    uint8_t role,
    uint8_t channel,
    uint8_t hardware_seed,
    uint32_t epoch)
{
    memset(node_snapshot, 0, sizeof(*node_snapshot));
    node_snapshot->state = NRF_TOPOLOGY_NODE_ASSIGNED;
    node_snapshot->hardware_id_length = 4U;
    for (unsigned index = 0U; index < 4U; ++index) {
        node_snapshot->hardware_id[index] = (uint8_t)(hardware_seed + index);
    }
    node_snapshot->node_id = role == NRF_UPSTREAM_ROLE_MAIN ? 0U : (uint8_t)(channel + 1U);
    node_snapshot->mux_channel = role == NRF_UPSTREAM_ROLE_MAIN
        ? NRF_UPSTREAM_MUX_CHANNEL_NONE
        : channel;
    node_snapshot->current_address = role == NRF_UPSTREAM_ROLE_MAIN
        ? NRF_UPSTREAM_ADDRESS_MAIN
        : NRF_UPSTREAM_ADDRESS_REMOTE;
    node_snapshot->protocol_result = NRF_UPSTREAM_RESULT_OK;
    node_snapshot->assignment_request_id = epoch;
}

static void status(
    nrf_upstream_status_t *status_frame,
    const nrf_topology_node_snapshot_t *node_snapshot,
    uint8_t role)
{
    memset(status_frame, 0, sizeof(*status_frame));
    status_frame->request_id = node_snapshot->assignment_request_id;
    status_frame->hardware_id_length = node_snapshot->hardware_id_length;
    memcpy(status_frame->hardware_id, node_snapshot->hardware_id, sizeof(status_frame->hardware_id));
    status_frame->role = role;
    status_frame->node_id = node_snapshot->node_id;
    status_frame->mux_channel = node_snapshot->mux_channel;
    status_frame->target_address = node_snapshot->current_address;
    status_frame->lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status_frame->result = NRF_UPSTREAM_RESULT_OK;
    status_frame->flags =
        NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
}

static void topo(bus_t *bus, nrf_topology_snapshot_t *topology, uint8_t channel)
{
    memset(bus, 0, sizeof(*bus));
    memset(topology, 0, sizeof(*topology));
    bus->sel = -1;
    bus->ch = channel;
    bus->remote_epoch = (uint32_t)(0x2200U + channel);
    bus->remote_hw = (uint8_t)(0x30U + channel);
    bus->snapshot_sequence = 42U;
    bus->source_uptime_ms = 1234U;
    topology->generation = 1U;
    topology->tca_available = true;
    node(&topology->main, NRF_UPSTREAM_ROLE_MAIN, 0U, 0x10U, 0x1111U);
    for (unsigned index = 0U; index < NRF_TOPOLOGY_REMOTE_COUNT; ++index) {
        topology->remotes[index].state = NRF_TOPOLOGY_NODE_ABSENT;
    }
    node(
        &topology->remotes[channel],
        NRF_UPSTREAM_ROLE_REMOTE,
        channel,
        bus->remote_hw,
        bus->remote_epoch);
}

static esp_err_t select_mux(void *context, uint8_t channel)
{
    bus_t *bus = context;
    ++bus->select;
    record_event(bus, E_SELECT);
    assert(channel == bus->ch);
    if (bus->fail_select) {
        return ESP_FAIL;
    }
    bus->sel = channel;
    return ESP_OK;
}

static esp_err_t close_mux(void *context)
{
    bus_t *bus = context;
    ++bus->close;
    record_event(bus, E_CLOSE);
    bus->sel = -1;
    return bus->fail_close ? ESP_FAIL : ESP_OK;
}

static bool mux_faulted(void *context)
{
    return ((bus_t *)context)->mux_fault;
}

static esp_err_t read_status(void *context, uint8_t address, nrf_upstream_status_t *status_frame)
{
    bus_t *bus = context;
    nrf_topology_node_snapshot_t node_snapshot;

    if (address == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        ++bus->discovery;
        return ESP_FAIL;
    }
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        ++bus->main_reads;
        record_event(bus, E_MAIN_READ);
        if (bus->main_absent) {
            return ESP_ERR_NOT_FOUND;
        }
        node(
            &node_snapshot,
            NRF_UPSTREAM_ROLE_MAIN,
            0U,
            0x10U,
            bus->main_bad_epoch ? 0x9999U : 0x1111U);
        status(status_frame, &node_snapshot, NRF_UPSTREAM_ROLE_MAIN);
        return ESP_OK;
    }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE) {
        ++bus->remote_reads;
        record_event(bus, E_REMOTE_READ);
        if (bus->mode == M_NACK) {
            return ESP_ERR_NOT_FOUND;
        }
        node(
            &node_snapshot,
            NRF_UPSTREAM_ROLE_REMOTE,
            bus->ch,
            bus->remote_hw,
            bus->remote_epoch);
        status(status_frame, &node_snapshot, NRF_UPSTREAM_ROLE_REMOTE);
        if (bus->mode == M_STATUS_BOOTING) {
            status_frame->lifecycle = NRF_UPSTREAM_LIFECYCLE_BOOTING;
        }
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t write_refresh(void *context, const nrf_upstream_sensor_refresh_t *request)
{
    bus_t *bus = context;
    ++bus->writes;
    record_event(bus, E_WRITE);
    bus->req = request->request_id;
    return ESP_OK;
}

static esp_err_t read_snapshot(
    void *context,
    nrf_upstream_status_t *status_frame,
    nrf_upstream_sensor_snapshot_t *snapshot)
{
    bus_t *bus = context;
    nrf_topology_node_snapshot_t node_snapshot;
    ++bus->snaps;
    record_event(bus, E_SNAPSHOT);

    if (bus->mode == M_PROTOCOL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    node(
        &node_snapshot,
        NRF_UPSTREAM_ROLE_REMOTE,
        bus->ch,
        bus->remote_hw,
        bus->remote_epoch);
    status(status_frame, &node_snapshot, NRF_UPSTREAM_ROLE_REMOTE);
    if (bus->mode == M_SNAPSHOT_IDENTITY_MISMATCH) {
        status_frame->hardware_id[0] ^= 0x5AU;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->sequence = bus->snapshot_sequence;
    snapshot->sample_complete_uptime_ms = bus->source_uptime_ms;

    if (bus->mode == M_FOREIGN || bus->mode == M_FOREIGN_TWICE) {
        if (bus->writes == 1U) {
            snapshot->request_id = 0U;
            snapshot->result = bus->snaps == 1U
                ? NRF_UPSTREAM_SENSOR_RESULT_PENDING
                : NRF_UPSTREAM_SENSOR_RESULT_OK;
            return ESP_OK;
        }
        if (bus->mode == M_FOREIGN_TWICE) {
            snapshot->request_id = 0U;
            snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_OK;
            return ESP_OK;
        }
    }

    snapshot->request_id = bus->req;
    switch (bus->mode) {
    case M_PENDING:
    case M_ABORT_AFTER_PENDING:
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_PENDING;
        break;
    case M_PARTIAL:
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_PARTIAL;
        snapshot->valid_mask = NRF_UPSTREAM_SENSOR_VALID_IMU;
        break;
    case M_NO_DATA:
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_NO_DATA;
        break;
    case M_BUSY:
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_BUSY;
        break;
    default:
        snapshot->result = NRF_UPSTREAM_SENSOR_RESULT_OK;
        snapshot->valid_mask = NRF_UPSTREAM_SENSOR_VALID_MASK;
        snapshot->pressure_pa = 101325U;
        break;
    }
    return ESP_OK;
}

static void sleep10(void *context, uint32_t delay_ms)
{
    bus_t *bus = context;
    assert(delay_ms == 10U);
    bus->now += delay_ms;
}

static uint64_t now_ms(void *context)
{
    return ((bus_t *)context)->now;
}

static bool aborted(void *context)
{
    const bus_t *bus = context;
    return bus->abort || (bus->mode == M_ABORT_AFTER_PENDING && bus->snaps >= 1U);
}

static nrf_sensor_transport_io_t make_io(bus_t *bus)
{
    return (nrf_sensor_transport_io_t){
        .context = bus,
        .mux_select = select_mux,
        .mux_close = close_mux,
        .mux_faulted = mux_faulted,
        .read_status = read_status,
        .write_sensor_refresh = write_refresh,
        .read_remote_snapshot = read_snapshot,
        .sleep_ms = sleep10,
        .now_ms = now_ms,
        .abort_requested = aborted,
    };
}

static nrf_sensor_transport_controller_t make_controller(void)
{
    nrf_sensor_transport_controller_t controller;
    const nrf_sensor_transport_config_t config = {
        .timeout_ms = 500U,
        .poll_interval_ms = 10U,
        .request_seed = 77U,
    };
    assert(nrf_sensor_transport_controller_init(&controller, &config) == ESP_OK);
    return controller;
}

static void init_cache(
    const nrf_topology_snapshot_t *topology,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    uint8_t channel)
{
    memset(cache, 0, sizeof(*cache) * NRF_TOPOLOGY_REMOTE_COUNT);
    assert(nrf_sensor_cache_reconcile_topology(topology, cache) == (uint8_t)(1U << channel));
}

static void assert_events(const bus_t *bus, const event_t *expected, unsigned count)
{
    assert(bus->event_count == count);
    for (unsigned index = 0U; index < count; ++index) {
        assert(bus->events[index] == expected[index]);
    }
}

static void test_cache_identity_lifecycle(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT] = {0};

    topo(&bus, &topology, 3U);
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 8U);
    cache[3].response_valid = true;
    cache[3].state = NRF_SENSOR_CACHE_OK;
    cache[3].last_attempt_state = NRF_SENSOR_CACHE_OK;
    cache[3].sensor.sequence = 100U;

    ++topology.generation;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 0U);
    assert(cache[3].response_valid && cache[3].sensor.sequence == 100U);

    topology.remotes[3].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 0U);
    assert(!cache[3].topology_current && cache[3].response_valid);

    topology.remotes[3].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 0U);
    assert(cache[3].topology_current && cache[3].state == NRF_SENSOR_CACHE_OK);

    topology.remotes[3].hardware_id[0] ^= 0x55U;
    ++topology.generation;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 8U);
    assert(cache[3].identity_valid && !cache[3].response_valid);
    assert(cache[3].state == NRF_SENSOR_CACHE_EMPTY);

    topo(&bus, &topology, 3U);
    memset(cache, 0, sizeof(cache));
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 8U);
    cache[3].response_valid = true;
    cache[3].sensor.sequence = 50U;
    topology.remotes[3].mux_channel = 4U;
    ++topology.generation;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 8U);
    assert(!cache[3].response_valid && cache[3].identity.mux_channel == 4U);

    cache[3].response_valid = true;
    topology.remotes[3].state = NRF_TOPOLOGY_NODE_ABSENT;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 0U);
    assert(!cache[3].identity_valid && !cache[3].response_valid);
    assert(cache[3].state == NRF_SENSOR_CACHE_EMPTY);
}

static void test_success_order_and_no_discovery(void)
{
    const event_t expected[] = {
        E_MAIN_READ, E_SELECT, E_REMOTE_READ, E_WRITE, E_SNAPSHOT, E_CLOSE,
    };

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        bus_t bus;
        nrf_topology_snapshot_t topology;
        nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
        topo(&bus, &topology, channel);
        init_cache(&topology, cache, channel);
        nrf_sensor_transport_controller_t controller = make_controller();
        nrf_sensor_transport_io_t io = make_io(&bus);
        bool reconcile = false;

        assert(nrf_sensor_transport_refresh_channel(
                   &controller, &io, &topology, channel, &cache[channel], &reconcile) == ESP_OK);
        assert(cache[channel].state == NRF_SENSOR_CACHE_OK);
        assert(cache[channel].sensor.pressure_pa == 101325U);
        assert(cache[channel].esp_receipt_uptime_ms == bus.now);
        assert(!reconcile && bus.discovery == 0U);
        assert(bus.select == 1U && bus.close == 1U);
        assert(bus.main_reads == 1U && bus.remote_reads == 1U && bus.writes == 1U);
        assert_events(&bus, expected, sizeof(expected) / sizeof(expected[0]));
    }

    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    topo(&bus, &topology, 2U);
    topology.main.state = NRF_TOPOLOGY_NODE_ABSENT;
    init_cache(&topology, cache, 2U);
    nrf_sensor_transport_controller_t controller = make_controller();
    nrf_sensor_transport_io_t io = make_io(&bus);
    bool reconcile = false;
    const event_t absent_main_expected[] = {
        E_SELECT, E_REMOTE_READ, E_WRITE, E_SNAPSHOT, E_CLOSE,
    };

    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 2U, &cache[2], &reconcile) == ESP_OK);
    assert(bus.main_reads == 0U && bus.discovery == 0U);
    assert_events(
        &bus,
        absent_main_expected,
        sizeof(absent_main_expected) / sizeof(absent_main_expected[0]));
}

static void test_final_result_mapping_and_timestamps(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    bool reconcile;
    nrf_sensor_transport_controller_t controller;
    nrf_sensor_transport_io_t io;

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.mode = M_PARTIAL;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].response_valid && cache[1].state == NRF_SENSOR_CACHE_PARTIAL);
    assert(cache[1].sensor.valid_mask == NRF_UPSTREAM_SENSOR_VALID_IMU);
    assert(bus.writes == 1U && bus.snaps == 1U && !reconcile);

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.mode = M_NO_DATA;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].response_valid && cache[1].state == NRF_SENSOR_CACHE_NO_DATA);
    assert(bus.writes == 1U && bus.snaps == 1U && !reconcile);

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.mode = M_BUSY;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].response_valid && cache[1].state == NRF_SENSOR_CACHE_SENSOR_BUSY);
    assert(bus.writes == 1U && bus.snaps == 1U && !reconcile);

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.now = 777U;
    bus.source_uptime_ms = 999999U;
    bus.snapshot_sequence = 7U;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].sensor.sample_complete_uptime_ms == 999999U);
    assert(cache[1].esp_receipt_uptime_ms == 777U);
    assert(cache[1].esp_receipt_uptime_ms != cache[1].sensor.sample_complete_uptime_ms);
}

static void test_status_cache_survives_busy_topology_refresh(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    nrf_sensor_transport_controller_t controller = make_controller();
    nrf_sensor_transport_io_t io = make_io(&bus);
    bool reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].upstream_status.valid);

    topology.bus_busy = true;
    topology.stale = true;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == 0U);
    assert(cache[1].upstream_status.valid);
    assert(cache[1].upstream_status.identity_matches);
    assert(cache[1].upstream_status.lifecycle == NRF_UPSTREAM_LIFECYCLE_READY);
    assert(cache[1].upstream_status.result == NRF_UPSTREAM_RESULT_OK);
    assert(cache[1].upstream_status.flags == (
        NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED));
    nrf_node_link_status_input_t input = {
        .assigned = true,
        .status_valid = cache[1].upstream_status.valid,
        .status_identity_matches = cache[1].upstream_status.identity_matches,
        .lifecycle = cache[1].upstream_status.lifecycle,
        .result = cache[1].upstream_status.result,
        .flags = cache[1].upstream_status.flags,
        .status_receipt_uptime_ms = cache[1].upstream_status.esp_receipt_uptime_ms,
    };
    assert(nrf_node_link_status_evaluate(
               &input,
               input.status_receipt_uptime_ms + NRF_NODE_LINK_STATUS_STALE_LIMIT_MS - 1U,
               false) == NRF_NODE_LINK_STATUS_READY);
    assert(nrf_node_link_status_evaluate(
               &input,
               input.status_receipt_uptime_ms + NRF_NODE_LINK_STATUS_STALE_LIMIT_MS,
               false) == NRF_NODE_LINK_STATUS_NOT_CONNECTED);
}

static void test_status_cache_retains_lifecycle_result_flags(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    topo(&bus, &topology, 2U);
    init_cache(&topology, cache, 2U);
    nrf_sensor_transport_controller_t controller = make_controller();
    nrf_sensor_transport_io_t io = make_io(&bus);
    bool reconcile = false;

    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 2U, &cache[2], &reconcile) == ESP_OK);
    assert(controller.main_status_cache.valid);
    assert(controller.main_status_cache.identity_matches);
    assert(controller.main_status_cache.lifecycle == NRF_UPSTREAM_LIFECYCLE_READY);
    assert(controller.main_status_cache.result == NRF_UPSTREAM_RESULT_OK);
    assert(controller.main_status_cache.flags == (
        NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED));
    assert(cache[2].upstream_status.valid);
    assert(cache[2].upstream_status.identity_matches);
    assert(cache[2].upstream_status.lifecycle == NRF_UPSTREAM_LIFECYCLE_READY);
    assert(cache[2].upstream_status.result == NRF_UPSTREAM_RESULT_OK);
    assert(cache[2].upstream_status.flags == (
        NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED));
    assert(cache[2].upstream_status.esp_receipt_uptime_ms == bus.now);
}

static void test_fresh_booting_status_is_retained_without_reconcile(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    topo(&bus, &topology, 3U);
    init_cache(&topology, cache, 3U);
    bus.mode = M_STATUS_BOOTING;
    nrf_sensor_transport_controller_t controller = make_controller();
    nrf_sensor_transport_io_t io = make_io(&bus);
    bool reconcile = false;

    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 3U, &cache[3], &reconcile) ==
        ESP_ERR_INVALID_STATE);
    assert(cache[3].upstream_status.valid);
    assert(cache[3].upstream_status.identity_matches);
    assert(cache[3].upstream_status.lifecycle == NRF_UPSTREAM_LIFECYCLE_BOOTING);
    assert(!reconcile && bus.writes == 0U && bus.close == 1U);
}

static void test_foreign_retry_and_second_conflict(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    bool reconcile;
    nrf_sensor_transport_controller_t controller;
    nrf_sensor_transport_io_t io;

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.mode = M_FOREIGN;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_OK);
    assert(cache[1].state == NRF_SENSOR_CACHE_OK);
    assert(bus.writes == 2U && bus.snaps == 3U && !reconcile);

    topo(&bus, &topology, 1U);
    init_cache(&topology, cache, 1U);
    bus.mode = M_FOREIGN_TWICE;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 1U, &cache[1], &reconcile) == ESP_ERR_INVALID_STATE);
    assert(cache[1].state == NRF_SENSOR_CACHE_SENSOR_BUSY);
    assert(bus.writes == 2U && bus.snaps == 3U && bus.close == 1U);
    assert(!reconcile);
}

static void test_timeout_abort_and_faults(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    bool reconcile;
    nrf_sensor_transport_controller_t controller;
    nrf_sensor_transport_io_t io;

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.mode = M_PENDING;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_TIMEOUT);
    assert(cache[4].state == NRF_SENSOR_CACHE_TIMEOUT && bus.now == 500U && bus.close == 1U);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.mode = M_ABORT_AFTER_PENDING;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_INVALID_STATE);
    assert(cache[4].state == NRF_SENSOR_CACHE_ABORTED);
    assert(bus.snaps == 1U && bus.close == 1U && !reconcile);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.mode = M_PROTOCOL;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_INVALID_RESPONSE);
    assert(cache[4].state == NRF_SENSOR_CACHE_PROTOCOL_ERROR && reconcile && bus.close == 1U);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.mode = M_NACK;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_NOT_FOUND);
    assert(cache[4].state == NRF_SENSOR_CACHE_IDENTITY_STALE);
    assert(!cache[4].topology_current && reconcile && bus.close == 1U);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.main_bad_epoch = true;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_INVALID_RESPONSE);
    assert(cache[4].state == NRF_SENSOR_CACHE_IDENTITY_STALE);
    assert(bus.remote_reads == 0U && reconcile && bus.select == 0U && bus.close == 0U);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.mode = M_SNAPSHOT_IDENTITY_MISMATCH;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_ERR_INVALID_RESPONSE);
    assert(cache[4].state == NRF_SENSOR_CACHE_IDENTITY_STALE);
    assert(!cache[4].topology_current && reconcile && bus.close == 1U);

    topo(&bus, &topology, 4U);
    init_cache(&topology, cache, 4U);
    bus.fail_close = true;
    bus.mux_fault = true;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 4U, &cache[4], &reconcile) == ESP_FAIL);
    assert(cache[4].state == NRF_SENSOR_CACHE_MUX_FAULT && reconcile);

    topo(&bus, &topology, 7U);
    init_cache(&topology, cache, 7U);
    bus.fail_select = true;
    controller = make_controller();
    io = make_io(&bus);
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 7U, &cache[7], &reconcile) == ESP_FAIL);
    assert(bus.close == 1U && bus.select == 1U && bus.main_reads == 1U);
    assert(bus.remote_reads == 0U && bus.discovery == 0U);
}

static void test_sequence_can_reset_after_new_assignment_epoch(void)
{
    bus_t bus;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_sensor_transport_controller_t controller;
    nrf_sensor_transport_io_t io;
    bool reconcile = false;

    topo(&bus, &topology, 5U);
    init_cache(&topology, cache, 5U);
    bus.snapshot_sequence = 100U;
    controller = make_controller();
    io = make_io(&bus);
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 5U, &cache[5], &reconcile) == ESP_OK);
    assert(cache[5].sensor.sequence == 100U);

    ++topology.generation;
    ++topology.remotes[5].assignment_request_id;
    bus.remote_epoch = topology.remotes[5].assignment_request_id;
    assert(nrf_sensor_cache_reconcile_topology(&topology, cache) == (uint8_t)(1U << 5U));
    assert(!cache[5].response_valid && cache[5].state == NRF_SENSOR_CACHE_EMPTY);

    bus.snapshot_sequence = 3U;
    reconcile = false;
    assert(nrf_sensor_transport_refresh_channel(
               &controller, &io, &topology, 5U, &cache[5], &reconcile) == ESP_OK);
    assert(cache[5].response_valid && cache[5].sensor.sequence == 3U);
    assert(cache[5].state == NRF_SENSOR_CACHE_OK);
}

int main(void)
{
    test_cache_identity_lifecycle();
    test_status_cache_survives_busy_topology_refresh();
    test_success_order_and_no_discovery();
    test_final_result_mapping_and_timestamps();
    test_status_cache_retains_lifecycle_result_flags();
    test_fresh_booting_status_is_retained_without_reconcile();
    test_foreign_retry_and_second_conflict();
    test_timeout_abort_and_faults();
    test_sequence_can_reset_after_new_assignment_epoch();
    return 0;
}
