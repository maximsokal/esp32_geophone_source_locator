#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "nrf_topology.h"
#include "nrf_topology_mode.h"
#include "nrf_topology_periods.h"
#include "sensor_bus.h"

/*
 * REMOTE_ONLY detection (4 REMOTE, no nRF MAIN). The reconcile tests run the real controller against a fake direct bus and
 * fake TCA channels; the classification tests use synthetic snapshots for states the fake bus cannot produce in one pass.
 */

typedef enum {
    MAIN_DIRECT_ABSENT,   /* 0x31 NACKs (ESP_ERR_NOT_FOUND) */
    MAIN_DIRECT_ASSIGNED, /* 0x31 answers as the assigned MAIN */
    MAIN_DIRECT_TIMEOUT,  /* 0x31 read fails with a transport error */
    MAIN_DIRECT_BUSY,     /* 0x31 read yields the shared bus */
} main_direct_t;

typedef struct {
    main_direct_t main_direct;
    bool remote_present[NRF_TOPOLOGY_REMOTE_COUNT];
    int selected;           /* -1: mux closed */
    uint64_t now_ms;
    unsigned abort_after;   /* abort_requested() turns true after this many calls; 0 = never */
    unsigned abort_calls;
} world_t;

static const uint8_t k_main_hw[4] = {0xA0, 0x00, 0x00, 0x01};

static void fill_status(nrf_upstream_status_t *status, uint8_t address, uint8_t role, uint8_t node_id,
                        uint8_t mux_channel, const uint8_t hw[4])
{
    memset(status, 0, sizeof(*status));
    status->hardware_id_length = 4U;
    memcpy(status->hardware_id, hw, 4U);
    status->role = role;
    status->node_id = node_id;
    status->mux_channel = mux_channel;
    status->target_address = address;
    status->lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status->result = NRF_UPSTREAM_RESULT_OK;
    status->flags = NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED | NRF_UPSTREAM_STATUS_FLAG_ASSIGNED;
}

static esp_err_t fake_mux_close(void *context)
{
    ((world_t *)context)->selected = -1;
    return ESP_OK;
}

static esp_err_t fake_mux_select(void *context, uint8_t channel)
{
    ((world_t *)context)->selected = (int)channel;
    return ESP_OK;
}

static esp_err_t fake_read_status(void *context, uint8_t address, nrf_upstream_status_t *status)
{
    world_t *world = context;
    if (address == NRF_UPSTREAM_ADDRESS_MAIN) {
        switch (world->main_direct) {
        case MAIN_DIRECT_ABSENT: return ESP_ERR_NOT_FOUND;
        case MAIN_DIRECT_TIMEOUT: return ESP_ERR_TIMEOUT;
        case MAIN_DIRECT_BUSY: return SENSOR_BUS_ERR_MUTEX_BUSY;
        case MAIN_DIRECT_ASSIGNED:
            fill_status(status, NRF_UPSTREAM_ADDRESS_MAIN, NRF_UPSTREAM_ROLE_MAIN, NRF_TOPOLOGY_MAIN_NODE_ID,
                        NRF_UPSTREAM_MUX_CHANNEL_NONE, k_main_hw);
            return ESP_OK;
        }
    }
    if (address == NRF_UPSTREAM_ADDRESS_REMOTE && world->selected >= 0 &&
        world->remote_present[world->selected]) {
        const uint8_t channel = (uint8_t)world->selected;
        const uint8_t hw[4] = {0xB0, channel, 0x00, 0x02};
        fill_status(status, NRF_UPSTREAM_ADDRESS_REMOTE, NRF_UPSTREAM_ROLE_REMOTE, (uint8_t)(channel + 1U), channel, hw);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t fake_write_assign(void *context, uint8_t address, const nrf_upstream_assign_t *assign)
{
    (void)context; (void)address; (void)assign;
    assert(!"no node in these scenarios needs an ASSIGN");
    return ESP_FAIL;
}

static uint64_t fake_now_ms(void *context) { return ((world_t *)context)->now_ms; }

static void fake_sleep_ms(void *context, uint32_t delay_ms) { ((world_t *)context)->now_ms += delay_ms; }

static bool fake_abort_requested(void *context)
{
    world_t *world = context;
    if (world->abort_after == 0U) return false;
    return ++world->abort_calls > world->abort_after;
}

static void world_init(world_t *world, main_direct_t main_direct, unsigned remotes)
{
    memset(world, 0, sizeof(*world));
    world->main_direct = main_direct;
    world->selected = -1;
    world->now_ms = 1000U;
    for (unsigned i = 0U; i < remotes && i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) world->remote_present[i] = true;
}

static nrf_topology_io_t io_for(world_t *world, bool tca_available, bool tca_fault)
{
    return (nrf_topology_io_t){
        .context = world, .tca_available = tca_available, .tca_fault = tca_fault,
        .mux_close = fake_mux_close, .mux_select = fake_mux_select, .read_status = fake_read_status,
        .write_assign = fake_write_assign, .now_ms = fake_now_ms, .sleep_ms = fake_sleep_ms,
        .abort_requested = fake_abort_requested,
    };
}

static void controller_init(nrf_topology_controller_t *controller)
{
    const nrf_topology_config_t config = {
        .confirm_timeout_ms = NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS,
        .poll_interval_ms = NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS,
        .request_seed = 1U,
    };
    assert(nrf_topology_controller_init(controller, &config) == ESP_OK);
}

static void reconcile(nrf_topology_controller_t *controller, world_t *world, bool tca_available, bool tca_fault,
                      nrf_topology_snapshot_t *snapshot)
{
    const nrf_topology_io_t io = io_for(world, tca_available, tca_fault);
    assert(nrf_topology_reconcile(controller, &io, snapshot) == ESP_OK);
}

static unsigned assigned_remotes(const nrf_topology_snapshot_t *snapshot)
{
    unsigned count = 0U;
    for (unsigned i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i)
        count += snapshot->remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED ? 1U : 0U;
    return count;
}

static nrf_topology_snapshot_t synthetic(nrf_topology_node_state_t main_state, unsigned remotes)
{
    nrf_topology_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.tca_available = true;
    snapshot.main.state = main_state;
    for (unsigned i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i)
        snapshot.remotes[i].state = i < remotes ? NRF_TOPOLOGY_NODE_ASSIGNED : NRF_TOPOLOGY_NODE_ABSENT;
    return snapshot;
}

/* ---- tests required by the TZ, stage 1 ---- */

static void remote_only_detection_four_remotes(void)
{
    world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
    world_init(&world, MAIN_DIRECT_ABSENT, 4U);
    controller_init(&controller);
    reconcile(&controller, &world, true, false, &snapshot);

    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(assigned_remotes(&snapshot) == 4U);
    for (unsigned i = 0U; i < 4U; ++i) assert(snapshot.remotes[i].node_id == i + 1U);

    const nrf_topology_mode_info_t info = nrf_topology_classify_mode(&snapshot);
    assert(info.mode == NRF_TOPOLOGY_MODE_REMOTE_ONLY);
    assert(info.remote_count == 4U);
    assert(info.complete);
    assert(strcmp(nrf_topology_mode_name(info.mode), "REMOTE_ONLY") == 0);
}

static void remote_only_two_or_three_remotes_are_explicitly_partial(void)
{
    for (unsigned remotes = 1U; remotes <= 3U; ++remotes) {
        world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
        world_init(&world, MAIN_DIRECT_ABSENT, remotes);
        controller_init(&controller);
        reconcile(&controller, &world, true, false, &snapshot);
        const nrf_topology_mode_info_t info = nrf_topology_classify_mode(&snapshot);
        assert(info.mode == NRF_TOPOLOGY_MODE_REMOTE_ONLY);
        assert(info.remote_count == remotes);
        assert(!info.complete);
    }
    /* Nothing assigned at all is no evidence of a mesh. */
    world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
    world_init(&world, MAIN_DIRECT_ABSENT, 0U);
    controller_init(&controller);
    reconcile(&controller, &world, true, false, &snapshot);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(nrf_topology_classify_mode(&snapshot).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);
}

static void main_absent_not_i2c_timeout(void)
{
    /* A MAIN read that times out or yields the bus is not an absent MAIN, even with four REMOTEs behind the mux. */
    const main_direct_t failures[] = {MAIN_DIRECT_TIMEOUT, MAIN_DIRECT_BUSY};
    for (unsigned f = 0U; f < 2U; ++f) {
        world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
        world_init(&world, failures[f], 4U);
        controller_init(&controller);
        reconcile(&controller, &world, true, false, &snapshot);
        assert(snapshot.main.state != NRF_TOPOLOGY_NODE_ABSENT);
        assert(nrf_topology_classify_mode(&snapshot).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);
    }
    const nrf_topology_node_state_t states[] = {
        NRF_TOPOLOGY_NODE_UNKNOWN, NRF_TOPOLOGY_NODE_CONFLICT, NRF_TOPOLOGY_NODE_PROTOCOL_ERROR,
        NRF_TOPOLOGY_NODE_TRANSPORT_ERROR,
    };
    for (unsigned s = 0U; s < sizeof(states) / sizeof(states[0]); ++s) {
        const nrf_topology_snapshot_t snapshot = synthetic(states[s], 4U);
        const nrf_topology_mode_info_t info = nrf_topology_classify_mode(&snapshot);
        assert(info.mode == NRF_TOPOLOGY_MODE_UNCERTAIN);
        assert(!info.complete);
    }
}

static void main_present_regression(void)
{
    for (unsigned remotes = 0U; remotes <= 4U; ++remotes) {
        world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
        world_init(&world, MAIN_DIRECT_ASSIGNED, remotes);
        controller_init(&controller);
        reconcile(&controller, &world, true, false, &snapshot);
        assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
        assert(assigned_remotes(&snapshot) == remotes);
        const nrf_topology_mode_info_t info = nrf_topology_classify_mode(&snapshot);
        assert(info.mode == NRF_TOPOLOGY_MODE_MAIN_PRESENT);
        assert(info.remote_count == remotes);
        assert(info.complete == (remotes == 4U));
    }
    assert(strcmp(nrf_topology_mode_name(NRF_TOPOLOGY_MODE_MAIN_PRESENT), "MAIN_PRESENT") == 0);
    assert(strcmp(nrf_topology_mode_name(NRF_TOPOLOGY_MODE_UNCERTAIN), "UNCERTAIN") == 0);
}

static void mainless_stale_not_ready(void)
{
    nrf_topology_snapshot_t snapshot = synthetic(NRF_TOPOLOGY_NODE_ABSENT, 4U);
    assert(nrf_topology_classify_mode(&snapshot).mode == NRF_TOPOLOGY_MODE_REMOTE_ONLY);

    snapshot.stale = true;
    nrf_topology_mode_info_t info = nrf_topology_classify_mode(&snapshot);
    assert(info.mode == NRF_TOPOLOGY_MODE_UNCERTAIN && !info.complete);

    snapshot.stale = false;
    snapshot.bus_busy = true;
    info = nrf_topology_classify_mode(&snapshot);
    assert(info.mode == NRF_TOPOLOGY_MODE_UNCERTAIN && !info.complete);
}

static void tca_fault_not_remote_only(void)
{
    /* Through the real controller: a TCA fault makes MAIN and every channel a transport error. */
    world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t snapshot;
    world_init(&world, MAIN_DIRECT_ABSENT, 4U);
    controller_init(&controller);
    reconcile(&controller, &world, true, true, &snapshot);
    assert(snapshot.tca_fault);
    assert(nrf_topology_classify_mode(&snapshot).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);

    nrf_topology_snapshot_t synthetic_fault = synthetic(NRF_TOPOLOGY_NODE_ABSENT, 4U);
    synthetic_fault.tca_fault = true;
    assert(nrf_topology_classify_mode(&synthetic_fault).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);

    nrf_topology_snapshot_t no_tca = synthetic(NRF_TOPOLOGY_NODE_ABSENT, 4U);
    no_tca.tca_available = false;
    assert(nrf_topology_classify_mode(&no_tca).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);

    assert(nrf_topology_classify_mode(NULL).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);
}

/* ---- D3: a REMOTE not reached by an interrupted scan is kept for the stale limit while MAIN is absent ---- */

static void interrupted_scan_second_pass(main_direct_t main_direct, uint64_t pause_ms, nrf_topology_snapshot_t *out)
{
    world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t first;
    world_init(&world, main_direct, 4U);
    controller_init(&controller);
    reconcile(&controller, &world, true, false, &first);
    assert(assigned_remotes(&first) == 4U);

    world.now_ms += pause_ms;
    world.abort_after = 1U;   /* the scan ends after channel 0 */
    world.abort_calls = 0U;
    reconcile(&controller, &world, true, false, out);
    assert(out->remotes[0].state == NRF_TOPOLOGY_NODE_ASSIGNED);
}

static void mainless_unknown_remotes_retained_within_stale_limit(void)
{
    nrf_topology_snapshot_t snapshot;
    interrupted_scan_second_pass(MAIN_DIRECT_ABSENT, 1000U, &snapshot);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ABSENT);
    assert(assigned_remotes(&snapshot) == 4U);
    for (unsigned i = 0U; i < 4U; ++i) assert(snapshot.remotes[i].node_id == i + 1U);
}

static void mainless_unknown_remotes_dropped_after_stale_limit(void)
{
    nrf_topology_snapshot_t snapshot;
    interrupted_scan_second_pass(MAIN_DIRECT_ABSENT, NRF_TOPOLOGY_STALE_LIMIT_MS, &snapshot);
    assert(snapshot.remotes[1].state == NRF_TOPOLOGY_NODE_UNKNOWN);
    assert(snapshot.remotes[3].state == NRF_TOPOLOGY_NODE_UNKNOWN);
}

static void legacy_main_assigned_retention_unchanged(void)
{
    nrf_topology_snapshot_t snapshot;
    interrupted_scan_second_pass(MAIN_DIRECT_ASSIGNED, 1000U, &snapshot);
    assert(snapshot.main.state == NRF_TOPOLOGY_NODE_ASSIGNED);
    assert(assigned_remotes(&snapshot) == 4U);
}

static void unknown_remotes_not_retained_when_main_read_fails(void)
{
    /* A MAIN transport error is not an absent MAIN: its REMOTEs were never scanned and must not be revived. */
    world_t world; nrf_topology_controller_t controller; nrf_topology_snapshot_t first, second;
    world_init(&world, MAIN_DIRECT_ABSENT, 4U);
    controller_init(&controller);
    reconcile(&controller, &world, true, false, &first);
    assert(assigned_remotes(&first) == 4U);
    world.now_ms += 1000U;
    world.main_direct = MAIN_DIRECT_TIMEOUT;
    reconcile(&controller, &world, true, false, &second);
    assert(second.main.state == NRF_TOPOLOGY_NODE_TRANSPORT_ERROR);
    assert(assigned_remotes(&second) == 0U);
    assert(nrf_topology_classify_mode(&second).mode == NRF_TOPOLOGY_MODE_UNCERTAIN);
}

/* ---- stage 3: the status node of an autonomous cycle ---- */

static void status_node_main_present_keeps_the_physical_main(void)
{
    const nrf_cs_node_id_t nodes[] = {1U, 0U, 3U, 2U, 4U};
    nrf_cs_node_id_t status = 0xEEU;
    assert(nrf_topology_select_status_node(true, NRF_TOPOLOGY_MAIN_NODE_ID, nodes, 5U, &status));
    assert(status == NRF_TOPOLOGY_MAIN_NODE_ID);
    /* An assigned MAIN that is not in this cycle is not asked. */
    const nrf_cs_node_id_t remotes[] = {2U, 3U};
    assert(nrf_topology_select_status_node(true, NRF_TOPOLOGY_MAIN_NODE_ID, remotes, 2U, &status));
    assert(status == 2U);
}

static void status_node_without_main_is_the_lowest_real_node(void)
{
    const nrf_cs_node_id_t four[] = {3U, 1U, 4U, 2U};
    nrf_cs_node_id_t status = 0xEEU;
    assert(nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, four, 4U, &status));
    assert(status == 1U); /* G1 */
    /* G1 not present: the lowest node that is, not a made-up G1. */
    const nrf_cs_node_id_t without_g1[] = {4U, 2U, 3U};
    assert(nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, without_g1, 3U, &status));
    assert(status == 2U);
    /* An ABSENT MAIN reports the unassigned id; it must not leak into the choice. */
    const nrf_cs_node_id_t one[] = {7U};
    assert(nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, one, 1U, &status));
    assert(status == 7U);
}

static void status_node_rejects_an_empty_cycle(void)
{
    nrf_cs_node_id_t status = 0x55U;
    const nrf_cs_node_id_t one[] = {1U};
    assert(!nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, NULL, 0U, &status));
    assert(!nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, one, 1U, NULL));
    assert(!nrf_topology_select_status_node(false, NRF_UPSTREAM_NODE_ID_UNASSIGNED, NULL, 1U, &status));
    assert(status == 0x55U); /* untouched on failure */
}

typedef struct { const char *name; void (*run)(void); } test_case_t;

#define CASE(fn) {#fn, fn}

/* No argument runs everything; one argument runs the named case (used to show a single RED case). */
int main(int argc, char **argv)
{
    static const test_case_t cases[] = {
        CASE(remote_only_detection_four_remotes),
        CASE(remote_only_two_or_three_remotes_are_explicitly_partial),
        CASE(main_absent_not_i2c_timeout),
        CASE(main_present_regression),
        CASE(mainless_stale_not_ready),
        CASE(tca_fault_not_remote_only),
        CASE(mainless_unknown_remotes_retained_within_stale_limit),
        CASE(mainless_unknown_remotes_dropped_after_stale_limit),
        CASE(legacy_main_assigned_retention_unchanged),
        CASE(unknown_remotes_not_retained_when_main_read_fails),
        CASE(status_node_main_present_keeps_the_physical_main),
        CASE(status_node_without_main_is_the_lowest_real_node),
        CASE(status_node_rejects_an_empty_cycle),
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
