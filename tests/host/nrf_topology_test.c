#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_topology.h"
#include "sensor_bus.h"
#include "tca9548a_mux.h"

typedef enum {
    FAKE_ABSENT = 0,
    FAKE_UNASSIGNED,
    FAKE_MAIN,
    FAKE_REMOTE,
} fake_node_mode_t;

typedef struct {
    fake_node_mode_t mode;
    uint8_t hw_len;
    uint8_t hw[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t node_id;
    uint8_t channel;
    uint32_t request_id;
    uint8_t result;
    bool first_readdress_fail;
    bool stay_pending;
    bool booting;
    unsigned assign_writes;
    uint32_t first_assign_request;
    uint32_t second_assign_request;
} fake_node_t;

typedef struct {
    bool tca_available;
    bool fail_initial_close;
    bool fail_close_after_select;
    bool transient_close_timeout;
    bool busy_next_close;
    bool use_verified_mux_close;
    uint8_t verified_mux_control;
    tca9548a_mux_t verified_mux;
    unsigned busy_count;
    int selected_channel;
    unsigned close_calls;
    unsigned select_calls;
    unsigned status_reads;
    unsigned discovery_reads_while_selected;
    unsigned collision_reads;
    unsigned sleeps;
    uint64_t now_ms;
    uint64_t assign_started_ms;
    uint32_t read_cost_ms;
    unsigned main_assign_order;
    unsigned first_select_order;
    unsigned order_counter;
    fake_node_t main;
    fake_node_t remote[NRF_TOPOLOGY_REMOTE_COUNT];
    bool extra_discovery[NRF_TOPOLOGY_REMOTE_COUNT];
    fake_node_t extra_discovery_node[NRF_TOPOLOGY_REMOTE_COUNT];
    bool main_reboots_on_channel;
    uint8_t reboot_channel;
    unsigned discovery_reads;
    unsigned main_reads;
    unsigned remote_reads;
    esp_err_t main_read_error;
    /* MAIN reads that fail while a channel is open: count left and the error to return. */
    unsigned main_open_channel_failures;
    esp_err_t main_open_channel_error;
    /* A socket whose wiring holds the bus: selecting the channel fails with this error. */
    esp_err_t select_error[NRF_TOPOLOGY_REMOTE_COUNT];
    unsigned selects_per_channel[NRF_TOPOLOGY_REMOTE_COUNT];
} fake_bus_t;

static void fake_set_hw(fake_node_t *node, uint8_t value)
{
    node->hw_len = 4U;
    node->hw[0] = value;
    node->hw[1] = (uint8_t)(value + 1U);
    node->hw[2] = (uint8_t)(value + 2U);
    node->hw[3] = (uint8_t)(value + 3U);
}

static void fake_make_status(const fake_node_t *node, nrf_upstream_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->hardware_id_length = node->hw_len;
    memcpy(status->hardware_id, node->hw, sizeof(status->hardware_id));
    status->request_id = node->request_id;
    status->lifecycle = node->booting
        ? NRF_UPSTREAM_LIFECYCLE_BOOTING
        : NRF_UPSTREAM_LIFECYCLE_READY;
    status->result = node->result;
    status->flags = NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
    switch (node->mode) {
    case FAKE_UNASSIGNED:
        status->role = NRF_UPSTREAM_ROLE_UNASSIGNED;
        status->node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
        status->mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
        status->target_address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
        break;
    case FAKE_MAIN:
        status->role = NRF_UPSTREAM_ROLE_MAIN;
        status->node_id = node->node_id;
        status->mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
        status->target_address = NRF_UPSTREAM_ADDRESS_MAIN;
        status->flags |= NRF_UPSTREAM_STATUS_FLAG_ASSIGNED;
        break;
    case FAKE_REMOTE:
        status->role = NRF_UPSTREAM_ROLE_REMOTE;
        status->node_id = node->node_id;
        status->mux_channel = node->channel;
        status->target_address = NRF_UPSTREAM_ADDRESS_REMOTE;
        status->flags |= NRF_UPSTREAM_STATUS_FLAG_ASSIGNED;
        break;
    case FAKE_ABSENT:
    default:
        break;
    }
}

static esp_err_t fake_close(void *context)
{
    fake_bus_t *bus = context;
    bus->close_calls++;
    if (bus->use_verified_mux_close) {
        return tca9548a_mux_close(&bus->verified_mux);
    }
    if (bus->busy_next_close) {
        bus->busy_next_close = false;
        bus->busy_count++;
        return SENSOR_BUS_ERR_MUTEX_BUSY;
    }
    if (bus->transient_close_timeout) {
        bus->transient_close_timeout = false;
        bus->close_calls++; /* retry close+verify confirms the closed state */
        bus->selected_channel = -1;
        return ESP_OK;
    }
    if (bus->fail_initial_close && bus->close_calls == 1U) {
        return ESP_FAIL;
    }
    if (bus->fail_close_after_select && bus->selected_channel >= 0) {
        return ESP_FAIL;
    }
    bus->selected_channel = -1;
    return ESP_OK;
}

static esp_err_t fake_verified_mux_write(
    void *context,
    uint8_t address,
    uint8_t value,
    tca9548a_mux_phase_t phase,
    int channel)
{
    fake_bus_t *bus = context;
    assert(address == 0x70U);
    (void)phase;
    (void)channel;
    bus->verified_mux_control = value;
    return ESP_OK;
}

static esp_err_t fake_verified_mux_read(
    void *context,
    uint8_t address,
    uint8_t *value,
    uint8_t expected,
    tca9548a_mux_phase_t phase,
    int channel)
{
    fake_bus_t *bus = context;
    assert(address == 0x70U);
    assert(value != NULL);
    (void)expected;
    (void)phase;
    (void)channel;
    if (bus->transient_close_timeout) {
        bus->transient_close_timeout = false;
        return ESP_ERR_TIMEOUT;
    }
    *value = bus->verified_mux_control;
    return ESP_OK;
}

static esp_err_t fake_select(void *context, uint8_t channel)
{
    fake_bus_t *bus = context;
    assert(channel < NRF_TOPOLOGY_REMOTE_COUNT);
    bus->select_calls++;
    bus->order_counter++;
    if (bus->first_select_order == 0U) {
        bus->first_select_order = bus->order_counter;
    }
    bus->selects_per_channel[channel]++;
    if (bus->select_error[channel] != ESP_OK) {
        return bus->select_error[channel];
    }
    bus->selected_channel = (int)channel;
    if (bus->main_reboots_on_channel && channel == bus->reboot_channel) {
        bus->main.mode = FAKE_UNASSIGNED;
        bus->main.node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
        bus->main.channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
        bus->main.request_id = 0U;
        bus->main.result = NRF_UPSTREAM_RESULT_OK;
        bus->main_reboots_on_channel = false;
    }
    return ESP_OK;
}

static fake_node_t *fake_visible_discovery(fake_bus_t *bus, unsigned *count)
{
    fake_node_t *visible = NULL;
    *count = 0U;
    if (bus->main.mode == FAKE_UNASSIGNED) {
        visible = &bus->main;
        (*count)++;
    }
    if (bus->selected_channel >= 0) {
        fake_node_t *remote = &bus->remote[bus->selected_channel];
        if (remote->mode == FAKE_UNASSIGNED) {
            visible = remote;
            (*count)++;
        }
        if (bus->extra_discovery[bus->selected_channel]) {
            visible = &bus->extra_discovery_node[bus->selected_channel];
            (*count)++;
        }
    }
    return visible;
}

static esp_err_t fake_read_status(void *context, uint8_t address, nrf_upstream_status_t *status)
{
    fake_bus_t *bus = context;
    bus->now_ms += bus->read_cost_ms;
    bus->status_reads++;
    if (address == NRF_UPSTREAM_ADDRESS_DISCOVERY) {
        bus->discovery_reads++;
        unsigned count = 0U;
        fake_node_t *node = fake_visible_discovery(bus, &count);
        if (bus->selected_channel >= 0) {
            bus->discovery_reads_while_selected++;
        }
        if (count > 1U) {
            bus->collision_reads++;
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (count == 0U || node == NULL) {
            return ESP_ERR_NOT_FOUND;
        }
        fake_make_status(node, status);
        return ESP_OK;
    }
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        bus->main_reads++;
        if (bus->selected_channel >= 0 && bus->main_open_channel_failures != 0U) {
            --bus->main_open_channel_failures;
            return bus->main_open_channel_error;
        }
        if (bus->main_read_error != ESP_OK) {
            return bus->main_read_error;
        }
        if (bus->main.mode != FAKE_MAIN) {
            return ESP_ERR_NOT_FOUND;
        }
        fake_make_status(&bus->main, status);
        return ESP_OK;
    }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE) {
        bus->remote_reads++;
        if (bus->selected_channel < 0) {
            return ESP_ERR_NOT_FOUND;
        }
        fake_node_t *remote = &bus->remote[bus->selected_channel];
        if (remote->mode != FAKE_REMOTE) {
            return ESP_ERR_NOT_FOUND;
        }
        fake_make_status(remote, status);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t fake_write_assign(void *context, uint8_t address, const nrf_upstream_assign_t *assign)
{
    fake_bus_t *bus = context;
    bus->assign_started_ms = bus->now_ms;
    unsigned count = 0U;
    fake_node_t *node;
    assert(address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    node = fake_visible_discovery(bus, &count);
    if (count != 1U || node == NULL) {
        return ESP_FAIL;
    }
    if (assign->hardware_id_length != node->hw_len ||
        memcmp(assign->hardware_id, node->hw, node->hw_len) != 0) {
        return ESP_FAIL;
    }
    node->assign_writes++;
    if (node->assign_writes == 1U) {
        node->first_assign_request = assign->request_id;
    }
    else if (node->assign_writes == 2U) {
        node->second_assign_request = assign->request_id;
    }
    node->request_id = assign->request_id;

    bus->order_counter++;
    if (node == &bus->main && bus->main_assign_order == 0U) {
        bus->main_assign_order = bus->order_counter;
    }

    if (node->first_readdress_fail && node->assign_writes == 1U) {
        node->result = NRF_UPSTREAM_RESULT_READDRESS_FAILED;
        return ESP_OK;
    }
    if (node->stay_pending) {
        node->result = NRF_UPSTREAM_RESULT_PENDING;
        return ESP_OK;
    }

    node->result = NRF_UPSTREAM_RESULT_OK;
    if (assign->role == NRF_UPSTREAM_ROLE_MAIN) {
        node->mode = FAKE_MAIN;
        node->node_id = 0U;
        node->channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    }
    else {
        node->mode = FAKE_REMOTE;
        node->node_id = assign->node_id;
        node->channel = assign->mux_channel;
    }
    return ESP_OK;
}

static void fake_sleep(void *context, uint32_t delay_ms)
{
    fake_bus_t *bus = context;
    assert(delay_ms > 0U && delay_ms <= 10U);
    bus->sleeps++;
    bus->now_ms += delay_ms;
}

static uint64_t fake_now_ms(void *context) { return ((fake_bus_t *)context)->now_ms; }

static nrf_topology_controller_t make_controller(void)
{
    nrf_topology_controller_t controller;
    const nrf_topology_config_t config = {
        .confirm_timeout_ms = 500U,
        .poll_interval_ms = 10U,
        .request_seed = 100U,
    };
    assert(nrf_topology_controller_init(&controller, &config) == ESP_OK);
    return controller;
}

static nrf_topology_io_t make_io(fake_bus_t *bus)
{
    const nrf_topology_io_t io = {
        .context = bus,
        .tca_available = bus->tca_available,
        .tca_fault = false,
        .mux_close = fake_close,
        .mux_select = fake_select,
        .read_status = fake_read_status,
        .write_assign = fake_write_assign,
        .now_ms = fake_now_ms,
        .sleep_ms = fake_sleep,
    };
    return io;
}

static void init_bus(fake_bus_t *bus, bool tca_available)
{
    memset(bus, 0, sizeof(*bus));
    bus->tca_available = tca_available;
    bus->selected_channel = -1;
}

static void test_main_only_without_tca_assigns_safely(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, false);
    bus.main.mode = FAKE_UNASSIGNED;
    fake_set_hw(&bus.main, 0x10U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.main.node_id == 0U);
    assert(bus.select_calls == 0U);
    assert(bus.main.assign_writes == 1U);
    for (size_t i = 0; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_ABSENT);
    }
}

static void test_main_is_assigned_before_any_channel_opens(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_UNASSIGNED;
    fake_set_hw(&bus.main, 0x20U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(bus.main_assign_order != 0U);
    assert(bus.first_select_order != 0U);
    assert(bus.main_assign_order < bus.first_select_order);
    assert(bus.collision_reads == 0U);
    assert(bus.selected_channel == -1);
    assert(bus.select_calls == 8U);
}

static void test_warm_reset_accepts_existing_main_and_all_remotes(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x30U);
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        bus.remote[i].mode = FAKE_REMOTE;
        bus.remote[i].node_id = (uint8_t)(i + 1U);
        bus.remote[i].channel = i;
        bus.remote[i].result = NRF_UPSTREAM_RESULT_OK;
        fake_set_hw(&bus.remote[i], (uint8_t)(0x40U + i * 4U));
    }
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(snapshot.remotes[i].node_id == (uint8_t)(i + 1U));
        assert(snapshot.remotes[i].mux_channel == i);
        assert(bus.remote[i].assign_writes == 0U);
    }
}

static void test_unassigned_remotes_get_stable_slot_ids(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x70U);
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        bus.remote[i].mode = FAKE_UNASSIGNED;
        bus.remote[i].result = NRF_UPSTREAM_RESULT_OK;
        fake_set_hw(&bus.remote[i], (uint8_t)(0x80U + i));
    }
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(bus.remote[i].mode == FAKE_REMOTE);
        assert(bus.remote[i].node_id == (uint8_t)(i + 1U));
        assert(bus.remote[i].channel == i);
    }
    assert(bus.selected_channel == -1);
}

static void test_duplicate_hardware_ids_mark_both_conflict(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x90U);
    for (uint8_t i = 0U; i < 2U; ++i) {
        bus.remote[i].mode = FAKE_REMOTE;
        bus.remote[i].node_id = (uint8_t)(i + 1U);
        bus.remote[i].channel = i;
        bus.remote[i].result = NRF_UPSTREAM_RESULT_OK;
        fake_set_hw(&bus.remote[i], 0xA0U);
    }
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[0].conflict_reason == NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID);
    assert(snapshot.remotes[1].conflict_reason == NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID);
}

static void test_stale_remote_assignment_is_conflict(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0xB0U);
    bus.remote[3].mode = FAKE_REMOTE;
    bus.remote[3].node_id = 2U;
    bus.remote[3].channel = 1U;
    bus.remote[3].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.remote[3], 0xC0U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.remotes[3].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[3].conflict_reason == NRF_TOPOLOGY_CONFLICT_REMOTE_IDENTITY);
}

static void test_readdress_failed_retries_same_request_id_once(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, false);
    bus.main.mode = FAKE_UNASSIGNED;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    bus.main.first_readdress_fail = true;
    fake_set_hw(&bus.main, 0xD0U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(bus.main.assign_writes == 2U);
    assert(bus.main.first_assign_request != 0U);
    assert(bus.main.first_assign_request == bus.main.second_assign_request);
}

static void test_initial_mux_close_failure_blocks_all_nrf_reads(void) /* except read-only MAIN 0x31 */
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.fail_initial_close = true;
    bus.main.mode = FAKE_UNASSIGNED;
    fake_set_hw(&bus.main, 0xE0U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    /* Only the read-only MAIN status at 0x31 is allowed; 0x30/0x32 and every
     * write stay blocked because a channel may still be open. */
    assert(bus.status_reads == 1U);
    assert(bus.main_reads == 1U);
    assert(bus.discovery_reads == 0U);
    assert(bus.remote_reads == 0U);
    assert(bus.main.assign_writes == 0U);
    assert(bus.select_calls == 0U);
    assert(bus.close_calls == 1U);
    for (size_t i = 0; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    }
}

static void test_initial_mux_close_failure_still_reports_assigned_main(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.fail_initial_close = true;
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xE8U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.main.current_address == NRF_UPSTREAM_ADDRESS_MAIN);
    assert(bus.main_reads == 1U);
    assert(bus.discovery_reads == 0U);
    assert(bus.remote_reads == 0U);
    assert(bus.select_calls == 0U);
    for (size_t i = 0; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    }
}

static void test_initial_mux_close_failure_main_read_errors_stay_truthful(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();

    init_bus(&bus, true);
    bus.fail_initial_close = true;
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xF0U);
    bus.main_read_error = ESP_ERR_TIMEOUT;
    nrf_topology_io_t io = make_io(&bus);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(!snapshot.bus_busy);
    assert(bus.discovery_reads == 0U);
    assert(bus.remote_reads == 0U);

    init_bus(&bus, true);
    bus.fail_initial_close = true;
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xF8U);
    bus.main_read_error = SENSOR_BUS_ERR_MUTEX_BUSY;
    io = make_io(&bus);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(snapshot.bus_busy);
    assert(bus.discovery_reads == 0U);
    assert(bus.remote_reads == 0U);
}

static void test_mux_stuck_open_after_remote_scan_keeps_main_visible(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x67U);
    bus.fail_close_after_select = true;
    nrf_topology_io_t io = make_io(&bus);

    /* Pass 1: the close after the first channel fails, leaving it open. */
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(bus.selected_channel == 0);
    const unsigned discovery_before = bus.discovery_reads;
    const unsigned remote_before = bus.remote_reads;
    const unsigned selects_before = bus.select_calls;

    /* Pass 2 with the channel still open: MAIN is read at 0x31 only. */
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(bus.discovery_reads == discovery_before);
    assert(bus.remote_reads == remote_before);
    assert(bus.select_calls == selects_before);
    assert(bus.collision_reads == 0U);
    for (size_t i = 0; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    }
}

static void test_main_reboot_during_remote_scan_closes_before_discovery_reconcile(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x11U);
    bus.main_reboots_on_channel = true;
    bus.reboot_channel = 2U;
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.main.mode == FAKE_MAIN);
    assert(bus.selected_channel == -1);
    assert(bus.main.assign_writes == 1U);
    assert(bus.collision_reads == 0U);
    assert(bus.select_calls == 3U);
}

static void test_remote_discovery_and_runtime_same_channel_is_conflict(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x22U);
    bus.remote[4].mode = FAKE_REMOTE;
    bus.remote[4].node_id = 5U;
    bus.remote[4].channel = 4U;
    bus.remote[4].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.remote[4], 0x33U);
    bus.extra_discovery[4] = true;
    bus.extra_discovery_node[4].mode = FAKE_UNASSIGNED;
    bus.extra_discovery_node[4].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.extra_discovery_node[4], 0x44U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.remotes[4].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[4].conflict_reason ==
           NRF_TOPOLOGY_CONFLICT_REMOTE_DISCOVERY_AND_RUNTIME);
}

static void test_pending_assignment_times_out_with_bounded_polls(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller;
    const nrf_topology_config_t config = {
        .confirm_timeout_ms = 30U,
        .poll_interval_ms = 10U,
        .request_seed = 9U,
    };
    assert(nrf_topology_controller_init(&controller, &config) == ESP_OK);
    init_bus(&bus, false);
    bus.main.mode = FAKE_UNASSIGNED;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    bus.main.stay_pending = true;
    fake_set_hw(&bus.main, 0x55U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(bus.sleeps == 3U);
}

static void test_close_failure_after_remote_operation_sets_mux_fault(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0x66U);
    bus.fail_close_after_select = true;
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(bus.select_calls == 1U);
}

static void test_absent_main_allows_diagnostic_remote_assignment(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.remote[0].mode = FAKE_UNASSIGNED;
    bus.remote[0].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.remote[0], 0x77U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[0].node_id == 1U);
}

static void test_mutex_busy_does_not_fault_mux_and_retries_next_window(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.busy_next_close = true;
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0xD8U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.bus_busy);
    assert(!snapshot.tca_fault);
    assert(bus.busy_count == 1U);
    assert(bus.status_reads == 0U);

    io = make_io(&bus);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(!snapshot.bus_busy);
    assert(!snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
}

static void test_single_transient_close_timeout_preserves_assigned_nodes(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.main, 0xE8U);
    bus.remote[0].mode = FAKE_REMOTE;
    bus.remote[0].node_id = 1U;
    bus.remote[0].channel = 0U;
    bus.remote[0].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.remote[0], 0xF0U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);

    const tca9548a_mux_io_t mux_io = {
        .write_control = fake_verified_mux_write,
        .read_control = fake_verified_mux_read,
        .context = &bus,
        .address = 0x70U,
    };
    assert(tca9548a_mux_init(&bus.verified_mux, &mux_io) == ESP_OK);
    bus.use_verified_mux_close = true;
    bus.transient_close_timeout = true;
    io = make_io(&bus);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(!snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);

    io = make_io(&bus);
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(!snapshot.tca_fault);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
}

static void assert_mux_fault_leaves_mux_and_discovery_untouched(
    const fake_bus_t *bus,
    const nrf_topology_snapshot_t *snapshot)
{
    assert(snapshot->tca_fault);
    assert(bus->select_calls == 0U);
    assert(bus->close_calls == 0U);
    assert(bus->discovery_reads == 0U);
    assert(bus->remote_reads == 0U);
    for (size_t i = 0; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        assert(snapshot->remotes[i].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    }
}

static void test_mux_fault_still_reports_assigned_main_read_only(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xA0U);
    nrf_topology_io_t io = make_io(&bus);
    io.tca_fault = true;

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.main.current_address == NRF_UPSTREAM_ADDRESS_MAIN);
    assert(bus.main_reads == 1U);
    assert(bus.main.assign_writes == 0U);
    assert_mux_fault_leaves_mux_and_discovery_untouched(&bus, &snapshot);
}

static void test_mux_fault_never_assigns_or_reads_unassigned_main(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_UNASSIGNED;
    fake_set_hw(&bus.main, 0xA8U);
    nrf_topology_io_t io = make_io(&bus);
    io.tca_fault = true;

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(bus.main_reads == 1U);
    assert(bus.main.assign_writes == 0U);
    assert_mux_fault_leaves_mux_and_discovery_untouched(&bus, &snapshot);
}

static void test_mux_fault_main_read_errors_keep_truthful_state(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();

    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xB0U);
    bus.main_read_error = ESP_ERR_TIMEOUT;
    nrf_topology_io_t io = make_io(&bus);
    io.tca_fault = true;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(!snapshot.bus_busy);
    assert_mux_fault_leaves_mux_and_discovery_untouched(&bus, &snapshot);

    init_bus(&bus, true);
    bus.main.mode = FAKE_MAIN;
    fake_set_hw(&bus.main, 0xB8U);
    bus.main_read_error = SENSOR_BUS_ERR_MUTEX_BUSY;
    io = make_io(&bus);
    io.tca_fault = true;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(snapshot.bus_busy);
    assert_mux_fault_leaves_mux_and_discovery_untouched(&bus, &snapshot);
}

static void test_assignment_deadline_includes_io_and_late_completion_recovers(void)
{
    fake_bus_t bus;
    init_bus(&bus, false);
    bus.main.mode = FAKE_UNASSIGNED;
    bus.main.stay_pending = true;
    fake_set_hw(&bus.main, 0x42U);
    bus.read_cost_ms = 120U;
    nrf_topology_controller_t controller = make_controller();
    nrf_topology_io_t io = make_io(&bus);
    nrf_topology_snapshot_t snapshot;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(bus.main.assign_writes == 1U);
    assert(bus.now_ms - bus.assign_started_ms <= 500U + bus.read_cost_ms);
    /* The deferred node finishes after the host deadline; the next pass accepts
     * its verified runtime identity without another ASSIGN or a reboot. */
    bus.read_cost_ms = 0U;
    bus.main.stay_pending = false;
    bus.main.mode = FAKE_MAIN;
    bus.main.node_id = NRF_TOPOLOGY_MAIN_NODE_ID;
    bus.main.channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    bus.main.result = NRF_UPSTREAM_RESULT_OK;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(bus.main.assign_writes == 1U);
}

static void setup_assigned_main_and_remotes(fake_bus_t *bus)
{
    init_bus(bus, true);
    bus->main.mode = FAKE_MAIN;
    bus->main.result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus->main, 0x50U);
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        bus->remote[i].mode = FAKE_REMOTE;
        bus->remote[i].node_id = (uint8_t)(i + 1U);
        bus->remote[i].channel = i;
        bus->remote[i].result = NRF_UPSTREAM_RESULT_OK;
        fake_set_hw(&bus->remote[i], (uint8_t)(0x60U + i * 4U));
    }
}

/*
 * The MAIN nRF keeps the result of the last rejected request (e.g. a garbled
 * frame after an aborted read) in STATUS. That is not an identity problem:
 * MAIN must stay ASSIGNED and the REMOTE scan must still run.
 */
static void test_main_with_stale_request_result_stays_assigned(void)
{
    static const uint8_t stale_results[] = {
        NRF_UPSTREAM_RESULT_INVALID_FRAME,
        NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE,
        NRF_UPSTREAM_RESULT_BUSY,
        NRF_UPSTREAM_RESULT_PENDING,
    };
    for (size_t index = 0U; index < sizeof(stale_results); ++index) {
        fake_bus_t bus;
        nrf_topology_snapshot_t snapshot;
        nrf_topology_controller_t controller = make_controller();
        setup_assigned_main_and_remotes(&bus);
        bus.main.result = stale_results[index];
        nrf_topology_io_t io = make_io(&bus);

        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(snapshot.main.conflict_reason == NRF_TOPOLOGY_CONFLICT_NONE);
        assert(snapshot.main.protocol_result == stale_results[index]);
        assert(bus.main.assign_writes == 0U);
        assert(bus.select_calls == NRF_TOPOLOGY_REMOTE_COUNT);
        for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
            assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED);
        }
    }
}

/*
 * Same rule for REMOTE: an already assigned node with the right role, node_id,
 * mux channel and ASSIGNED flag keeps its slot whatever the last request left
 * in STATUS.result (field log 2026-10-25: both REMOTE went CONFLICT while still
 * answering at 0x32).
 */
static void test_remote_with_stale_request_result_stays_assigned(void)
{
    static const uint8_t stale_results[] = {
        NRF_UPSTREAM_RESULT_INVALID_FRAME,
        NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE,
        NRF_UPSTREAM_RESULT_BUSY,
        NRF_UPSTREAM_RESULT_PENDING,
    };
    for (size_t index = 0U; index < sizeof(stale_results); ++index) {
        fake_bus_t bus;
        nrf_topology_snapshot_t snapshot;
        nrf_topology_controller_t controller = make_controller();
        setup_assigned_main_and_remotes(&bus);
        for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
            bus.remote[i].result = stale_results[index];
        }
        nrf_topology_io_t io = make_io(&bus);

        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
        for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
            assert(snapshot.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED);
            assert(snapshot.remotes[i].conflict_reason == NRF_TOPOLOGY_CONFLICT_NONE);
            assert(snapshot.remotes[i].protocol_result == stale_results[index]);
            assert(snapshot.remotes[i].node_id == (uint8_t)(i + 1U));
            assert(snapshot.remotes[i].mux_channel == i);
            assert(bus.remote[i].assign_writes == 0U);
        }
        assert(bus.selected_channel == -1);
    }
}

/* A stale result must not hide a real REMOTE identity or hardware-id problem. */
static void test_remote_stale_result_does_not_mask_real_conflicts(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    setup_assigned_main_and_remotes(&bus);
    bus.remote[0].result = NRF_UPSTREAM_RESULT_INVALID_FRAME;
    bus.remote[1].result = NRF_UPSTREAM_RESULT_INVALID_FRAME;
    bus.remote[1].node_id = 7U;           /* wrong slot id */
    bus.remote[2].result = NRF_UPSTREAM_RESULT_BUSY;
    bus.remote[2].channel = 5U;           /* claims another mux channel */
    bus.remote[3].result = NRF_UPSTREAM_RESULT_PENDING;
    bus.remote[4].result = NRF_UPSTREAM_RESULT_INVALID_FRAME;
    bus.remote[5].result = NRF_UPSTREAM_RESULT_INVALID_FRAME;
    fake_set_hw(&bus.remote[5], 0x80U);   /* same hardware id on 4 and 5 */
    fake_set_hw(&bus.remote[4], 0x80U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[1].conflict_reason == NRF_TOPOLOGY_CONFLICT_REMOTE_IDENTITY);
    assert(snapshot.remotes[1].node_id == 7U);
    assert(snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[2].conflict_reason == NRF_TOPOLOGY_CONFLICT_REMOTE_IDENTITY);
    assert(snapshot.remotes[3].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[4].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[5].state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.remotes[4].conflict_reason == NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID);
    assert(snapshot.remotes[5].conflict_reason == NRF_TOPOLOGY_CONFLICT_DUPLICATE_HARDWARE_ID);
}

/* A real identity problem must still block the remotes and say why. */
static void test_main_identity_mismatch_is_conflict_with_reason(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    setup_assigned_main_and_remotes(&bus);
    bus.main.node_id = 2U; /* answers at 0x31 but claims a REMOTE slot id */
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.main.conflict_reason == NRF_TOPOLOGY_CONFLICT_MAIN_IDENTITY);
    assert(snapshot.main.role == NRF_UPSTREAM_ROLE_MAIN);
    assert(snapshot.main.lifecycle == NRF_UPSTREAM_LIFECYCLE_READY);
    assert(snapshot.main.node_id == 2U);
    assert((snapshot.main.flags & NRF_UPSTREAM_STATUS_FLAG_ASSIGNED) != 0U);
    assert(bus.select_calls == 0U);
    assert(bus.main.assign_writes == 0U);
}

/* The same guard applies to a MAIN that is not (yet) a ready, unassigned node. */
static void test_main_discovery_not_ready_is_conflict_with_reason(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.main.mode = FAKE_UNASSIGNED;
    bus.main.booting = true;
    fake_set_hw(&bus.main, 0x51U);
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_CONFLICT);
    assert(snapshot.main.conflict_reason == NRF_TOPOLOGY_CONFLICT_DISCOVERY_NOT_UNASSIGNED);
    assert(snapshot.main.lifecycle == NRF_UPSTREAM_LIFECYCLE_BOOTING);
    assert(bus.select_calls == 0U);
    assert(bus.main.assign_writes == 0U);
}

/*
 * A single failed MAIN read while a channel is open (the I2C timeouts seen on the
 * board) is a transport problem, not "MAIN lost": the channel must not be
 * declared in conflict and the scan must go on.
 */
static void test_transient_main_read_error_during_scan_is_retried(void)
{
    static const esp_err_t errors[] = {ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE};

    for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        fake_bus_t bus;
        nrf_topology_snapshot_t snapshot;
        nrf_topology_controller_t controller = make_controller();

        setup_assigned_main_and_remotes(&bus);
        bus.main_open_channel_failures = 1U;
        bus.main_open_channel_error = errors[i];
        nrf_topology_io_t io = make_io(&bus);

        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(bus.main_open_channel_failures == 0U);
        assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            assert(snapshot.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED);
            assert(snapshot.remotes[channel].conflict_reason == 0U);
        }
        assert(bus.selected_channel == -1);
    }
}

/* MAIN that stays unreadable on an open channel is a transport error, never a conflict. */
static void test_persistent_main_read_error_during_scan_is_not_a_conflict(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();

    setup_assigned_main_and_remotes(&bus);
    bus.main_open_channel_failures = 100U;
    bus.main_open_channel_error = ESP_ERR_INVALID_STATE;
    nrf_topology_io_t io = make_io(&bus);

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        assert(snapshot.remotes[channel].state != NRF_TOPOLOGY_NODE_CONFLICT);
    }
    assert(bus.selected_channel == -1);
}

/* The bench of 2026-11-10: no MAIN nRF, nodes on channels 0, 2 and 3, channel 1 an empty socket that hangs the bus. */
static void init_bench_with_bad_channel_1(fake_bus_t *bus)
{
    static const uint8_t node_channels[] = { 0U, 2U, 3U };
    init_bus(bus, true);
    for (size_t i = 0U; i < sizeof(node_channels); ++i) {
        const uint8_t channel = node_channels[i];
        bus->remote[channel].mode = FAKE_REMOTE;
        bus->remote[channel].node_id = (uint8_t)(channel + 1U);
        bus->remote[channel].channel = channel;
        bus->remote[channel].result = NRF_UPSTREAM_RESULT_OK;
        fake_set_hw(&bus->remote[channel], (uint8_t)(0x40U + 0x10U * channel));
    }
    bus->select_error[1] = ESP_ERR_TIMEOUT;
}

static void assert_bench_nodes_assigned(const nrf_topology_snapshot_t *snapshot)
{
    assert(!snapshot->tca_fault && !snapshot->bus_busy);
    assert(snapshot->main.state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(snapshot->remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot->remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot->remotes[3].state == NRF_TOPOLOGY_NODE_ASSIGNED);
}

static void test_channel_that_hangs_the_select_is_not_selected_again(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 5000U;

    /* First pass: the bad socket is found once; the channels after it are still scanned. */
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 1U);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(snapshot.quarantined_channels == 0U);        /* it was selected in this pass */
    assert_bench_nodes_assigned(&snapshot);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) ==
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);

    /* The log showed a pass about every 8 s, each selecting channel 1 again. Now none of them does. */
    for (unsigned pass = 0U; pass < 7U; ++pass) {
        bus.now_ms += 8000U;
        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(bus.selects_per_channel[1] == 1U);
        assert(snapshot.quarantined_channels == 0x02U);
        assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_ABSENT);  /* an empty socket to every consumer */
        assert_bench_nodes_assigned(&snapshot);
        assert(bus.selects_per_channel[0] == pass + 2U);
        assert(bus.selects_per_channel[3] == pass + 2U);
    }
    assert(bus.selected_channel == -1);

    /* After the quarantine one probe is allowed; it fails again and the next wait is twice as long. */
    bus.now_ms = 5000U + NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 2U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) ==
        2U * NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);
    bus.now_ms += 2U * NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS - 1U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 2U);
    bus.now_ms += 1U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 3U);

    /*
     * The third hang of a socket that never answered switches it off until reboot: a further hang risks a bus that
     * stays dead until the mux is power-cycled, and no node was ever there to lose.
     */
    assert(NRF_TOPOLOGY_CHANNEL_HANG_LIMIT == 3U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) >
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS);
    for (unsigned pass = 0U; pass < 200U; ++pass) {
        bus.now_ms += 3600000U;                           /* 200 hours */
        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(bus.selects_per_channel[1] == 3U);
        assert(snapshot.quarantined_channels == 0x02U);
        assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_ABSENT);
        assert_bench_nodes_assigned(&snapshot);
    }
}

/* Board log 2026-11-13: channel 1 hung the bus, then a select worked, then it hung again. The ladder must not reset. */
static void test_intermittent_socket_is_not_forgiven_by_a_select_that_works(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 5000U;

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);          /* hang 1 */
    assert(controller.channel_select_failures[1] == 1U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) ==
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);

    bus.select_error[1] = ESP_OK;                                  /* the select happens to work, nothing answers */
    bus.now_ms += NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 2U);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(controller.channel_select_failures[1] == 1U);           /* an empty answer is not proof of a working socket */

    bus.select_error[1] = ESP_ERR_TIMEOUT;                         /* hang 2: the wait doubles */
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(controller.channel_select_failures[1] == 2U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) ==
        2U * NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);

    bus.select_error[1] = ESP_OK;
    bus.now_ms += 2U * NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(controller.channel_select_failures[1] == 2U);

    bus.select_error[1] = ESP_ERR_TIMEOUT;                         /* hang 3: off until reboot */
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(controller.channel_select_failures[1] == 3U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) >
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS);
    bus.select_error[1] = ESP_OK;
    bus.now_ms += 86400000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[1] == 5U);                      /* not selected again after hang 3 */
    assert(snapshot.quarantined_channels == 0x02U);
    assert_bench_nodes_assigned(&snapshot);
}

/* A node that answered on a channel earns it the right to keep being retried (never switched off for good). */
static void test_channel_that_once_carried_a_device_is_never_switched_off(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    bus.select_error[1] = ESP_OK;
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 1000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert_bench_nodes_assigned(&snapshot);
    assert(controller.channel_had_device[0] && controller.channel_had_device[2] && controller.channel_had_device[3]);
    assert(!controller.channel_had_device[1]);

    bus.select_error[2] = ESP_ERR_TIMEOUT;
    for (unsigned hang = 0U; hang < 12U; ++hang) {
        bus.now_ms += 8000U;
        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        const uint64_t remaining = nrf_topology_channel_quarantine_remaining_ms(&controller, 2U, bus.now_ms);
        assert(remaining <= NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS);
        bus.now_ms += remaining;
    }
    assert(controller.channel_select_failures[2] >= 3U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 2U, bus.now_ms) <=
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_MAX_MS);
}

/* A close that fails while a channel is selected is that channel's hang, not an unattributed fault. */
static void test_mux_close_failure_is_counted_against_the_selected_channel(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bus(&bus, true);
    bus.fail_close_after_select = true;
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 1000U;

    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.tca_fault);
    assert(bus.selects_per_channel[0] == 1U);
    assert(controller.channel_select_failures[0] == 1U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 0U, bus.now_ms) ==
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);
}

static void test_quarantined_channel_is_used_again_once_a_select_succeeds(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 1000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);

    /* The socket is repaired and a new node is plugged in while the channel is still quarantined. */
    bus.select_error[1] = ESP_OK;
    bus.remote[1].mode = FAKE_UNASSIGNED;
    bus.remote[1].result = NRF_UPSTREAM_RESULT_OK;
    fake_set_hw(&bus.remote[1], 0x90U);
    bus.now_ms += NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS - 1U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.quarantined_channels == 0x02U);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_ABSENT);

    bus.now_ms += 1U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(snapshot.quarantined_channels == 0U);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[1].node_id == 2U);
    assert(controller.channel_select_failures[1] == 0U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) == 0U);

    /* A later single failure of this now confirmed channel starts from the beginning. */
    bus.select_error[1] = ESP_ERR_TIMEOUT;
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) == 0U);
}

static void test_confirmed_node_channel_gets_one_retry_before_quarantine(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    bus.select_error[1] = ESP_OK;                       /* channel 1 is just empty here */
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 1000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert_bench_nodes_assigned(&snapshot);

    /* One timeout on a working node's channel is the shared bus being slow, not a bad socket. */
    bus.select_error[2] = ESP_ERR_TIMEOUT;
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 2U, bus.now_ms) == 0U);
    assert(snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);     /* identity retained */
    bus.select_error[2] = ESP_OK;
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[2] == 3U);
    assert(controller.channel_select_failures[2] == 0U);
    assert(snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_ASSIGNED);

    /* Two failures in a row are a bad channel: quarantined, the node becomes a transport error, never "absent". */
    bus.select_error[2] = ESP_ERR_TIMEOUT;
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 2U, bus.now_ms) == 0U);
    bus.now_ms += 8000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[2] == 5U);
    assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 2U, bus.now_ms) ==
        NRF_TOPOLOGY_CHANNEL_QUARANTINE_BASE_MS);
    bus.now_ms += 30000U;
    assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
    assert(bus.selects_per_channel[2] == 5U);
    assert(snapshot.quarantined_channels == 0x04U);
    assert(snapshot.remotes[2].state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(snapshot.remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(snapshot.remotes[3].state == NRF_TOPOLOGY_NODE_ASSIGNED);
}

static void test_busy_bus_on_select_is_not_a_bad_channel(void)
{
    fake_bus_t bus;
    nrf_topology_snapshot_t snapshot;
    nrf_topology_controller_t controller = make_controller();
    init_bench_with_bad_channel_1(&bus);
    bus.select_error[1] = SENSOR_BUS_ERR_MUTEX_BUSY;    /* another task owns the bus: nothing was selected */
    nrf_topology_io_t io = make_io(&bus);
    bus.now_ms = 1000U;

    for (unsigned pass = 0U; pass < 3U; ++pass) {
        assert(nrf_topology_reconcile(&controller, &io, &snapshot) == ESP_OK);
        assert(snapshot.bus_busy);
        assert(snapshot.quarantined_channels == 0U);
        assert(controller.channel_select_failures[1] == 0U);
        assert(nrf_topology_channel_quarantine_remaining_ms(&controller, 1U, bus.now_ms) == 0U);
        assert(bus.selects_per_channel[1] == pass + 1U);
        bus.now_ms += 8000U;
    }
}

int main(void)
{
    test_channel_that_hangs_the_select_is_not_selected_again();
    test_intermittent_socket_is_not_forgiven_by_a_select_that_works();
    test_channel_that_once_carried_a_device_is_never_switched_off();
    test_mux_close_failure_is_counted_against_the_selected_channel();
    test_quarantined_channel_is_used_again_once_a_select_succeeds();
    test_confirmed_node_channel_gets_one_retry_before_quarantine();
    test_busy_bus_on_select_is_not_a_bad_channel();
    test_transient_main_read_error_during_scan_is_retried();
    test_persistent_main_read_error_during_scan_is_not_a_conflict();
    test_main_with_stale_request_result_stays_assigned();
    test_remote_with_stale_request_result_stays_assigned();
    test_remote_stale_result_does_not_mask_real_conflicts();
    test_main_identity_mismatch_is_conflict_with_reason();
    test_main_discovery_not_ready_is_conflict_with_reason();
    test_main_only_without_tca_assigns_safely();
    test_main_is_assigned_before_any_channel_opens();
    test_warm_reset_accepts_existing_main_and_all_remotes();
    test_unassigned_remotes_get_stable_slot_ids();
    test_duplicate_hardware_ids_mark_both_conflict();
    test_stale_remote_assignment_is_conflict();
    test_readdress_failed_retries_same_request_id_once();
    test_initial_mux_close_failure_blocks_all_nrf_reads();
    test_initial_mux_close_failure_still_reports_assigned_main();
    test_initial_mux_close_failure_main_read_errors_stay_truthful();
    test_mux_stuck_open_after_remote_scan_keeps_main_visible();
    test_main_reboot_during_remote_scan_closes_before_discovery_reconcile();
    test_remote_discovery_and_runtime_same_channel_is_conflict();
    test_pending_assignment_times_out_with_bounded_polls();
    test_assignment_deadline_includes_io_and_late_completion_recovers();
    test_close_failure_after_remote_operation_sets_mux_fault();
    test_absent_main_allows_diagnostic_remote_assignment();
    test_mutex_busy_does_not_fault_mux_and_retries_next_window();
    test_single_transient_close_timeout_preserves_assigned_nodes();
    test_mux_fault_still_reports_assigned_main_read_only();
    test_mux_fault_never_assigns_or_reads_unassigned_main();
    test_mux_fault_main_read_errors_keep_truthful_state();
    return 0;
}
