#include "app_pod_status_inventory_policy.h"

#include "hw_inventory.h"


bool app_pod_status_bridge_report_topology_inventory(
    const nrf_topology_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->generation == 0U) {
        return false;
    }

    const hw_module_state_t mux_state = snapshot->tca_fault
        ? HW_STATE_FAULT
        : snapshot->tca_available
            ? HW_STATE_PRESENT
            : HW_STATE_ABSENT;
    hw_inventory_report(
        HW_MODULE_SENSOR_MUX,
        mux_state,
        snapshot->tca_fault
            ? "topology probe fault"
            : snapshot->tca_available
                ? "topology probe ok"
                : "no response");

    switch (snapshot->main.state) {
        case NRF_TOPOLOGY_NODE_ASSIGNED:
            hw_inventory_report(
                HW_MODULE_MAIN_XIAO,
                HW_STATE_PRESENT,
                "upstream target assigned");
            break;
        case NRF_TOPOLOGY_NODE_ABSENT:
            hw_inventory_report(
                HW_MODULE_MAIN_XIAO,
                HW_STATE_ABSENT,
                "no upstream response");
            break;
        case NRF_TOPOLOGY_NODE_UNKNOWN:
            break;
        default:
            hw_inventory_report(
                HW_MODULE_MAIN_XIAO,
                HW_STATE_FAULT,
                "upstream topology fault");
            break;
    }

    return true;
}
