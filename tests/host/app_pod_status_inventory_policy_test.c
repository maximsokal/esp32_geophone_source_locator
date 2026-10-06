#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "app_pod_status_inventory_policy.h"
#include "hw_inventory.h"

typedef struct {
    bool tca_available;
    bool tca_fault;
    hw_module_state_t expected_state;
    const char *expected_reason;
} mux_case_t;

typedef struct {
    nrf_topology_node_state_t state;
    hw_module_state_t expected_state;
    const char *expected_reason;
} main_case_t;

static void assert_inventory_state(
    hw_module_id_t id,
    hw_module_state_t expected_state,
    const char *expected_reason)
{
    hw_module_info_t module;
    assert(hw_inventory_get(id, &module));
    assert(module.state == expected_state);
    assert(strcmp(module.reason, expected_reason) == 0);
}

static void test_unresolved_topology_keeps_registry_unchanged(void)
{
    hw_inventory_init();
    hw_inventory_report(HW_MODULE_SENSOR_MUX, HW_STATE_DEGRADED, "mux kept");
    hw_inventory_report(HW_MODULE_MAIN_XIAO, HW_STATE_DEGRADED, "xiao kept");

    const nrf_topology_snapshot_t snapshot = {
        .generation = 0U,
        .tca_available = false,
        .tca_fault = false,
        .main = {.state = NRF_TOPOLOGY_NODE_ASSIGNED},
    };

    assert(!app_pod_status_bridge_report_topology_inventory(&snapshot));
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_DEGRADED, "mux kept");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_DEGRADED, "xiao kept");
    assert(!app_pod_status_bridge_report_topology_inventory(NULL));
}

static void test_all_mux_and_main_node_states(void)
{
    static const mux_case_t mux_cases[] = {
        {false, false, HW_STATE_ABSENT, "no response"},
        {true, false, HW_STATE_PRESENT, "topology probe ok"},
        {false, true, HW_STATE_FAULT, "topology probe fault"},
        {true, true, HW_STATE_FAULT, "topology probe fault"},
    };
    static const main_case_t main_cases[] = {
        {NRF_TOPOLOGY_NODE_ASSIGNED, HW_STATE_PRESENT, "upstream target assigned"},
        {NRF_TOPOLOGY_NODE_ABSENT, HW_STATE_ABSENT, "no upstream response"},
        {NRF_TOPOLOGY_NODE_UNKNOWN, HW_STATE_DEGRADED, "xiao kept"},
        {NRF_TOPOLOGY_NODE_TRANSPORT_ERROR, HW_STATE_FAULT, "upstream topology fault"},
    };

    for (
        size_t mux_index = 0U;
        mux_index < sizeof(mux_cases) / sizeof(mux_cases[0]);
        ++mux_index
    ) {
        for (
            size_t main_index = 0U;
            main_index < sizeof(main_cases) / sizeof(main_cases[0]);
            ++main_index
        ) {
            hw_inventory_init();
            hw_inventory_report(HW_MODULE_SENSOR_MUX, HW_STATE_DEGRADED, "mux kept");
            hw_inventory_report(HW_MODULE_MAIN_XIAO, HW_STATE_DEGRADED, "xiao kept");

            nrf_topology_snapshot_t snapshot = {
                .generation = 1U,
                .tca_available = mux_cases[mux_index].tca_available,
                .tca_fault = mux_cases[mux_index].tca_fault,
                .main = {.state = main_cases[main_index].state},
            };

            assert(app_pod_status_bridge_report_topology_inventory(&snapshot));
            assert_inventory_state(
                HW_MODULE_SENSOR_MUX,
                mux_cases[mux_index].expected_state,
                mux_cases[mux_index].expected_reason);
            assert_inventory_state(
                HW_MODULE_MAIN_XIAO,
                main_cases[main_index].expected_state,
                main_cases[main_index].expected_reason);
        }
    }
}

static void test_inventory_tracks_present_absent_present_transitions(void)
{
    hw_inventory_init();
    nrf_topology_snapshot_t snapshot = {
        .generation = 1U,
        .tca_available = true,
        .main = {.state = NRF_TOPOLOGY_NODE_ASSIGNED},
    };

    assert(app_pod_status_bridge_report_topology_inventory(&snapshot));
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, "topology probe ok");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_PRESENT, "upstream target assigned");

    snapshot.generation = 2U;
    snapshot.tca_available = false;
    snapshot.main.state = NRF_TOPOLOGY_NODE_ABSENT;
    assert(app_pod_status_bridge_report_topology_inventory(&snapshot));
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_ABSENT, "no response");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_ABSENT, "no upstream response");

    snapshot.generation = 3U;
    snapshot.tca_available = true;
    snapshot.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    assert(app_pod_status_bridge_report_topology_inventory(&snapshot));
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, "topology probe ok");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_PRESENT, "upstream target assigned");
}

int main(void)
{
    test_unresolved_topology_keeps_registry_unchanged();
    test_all_mux_and_main_node_states();
    test_inventory_tracks_present_absent_present_transitions();
    return 0;
}
