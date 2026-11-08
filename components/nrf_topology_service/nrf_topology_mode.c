#include "nrf_topology_mode.h"

nrf_topology_mode_info_t nrf_topology_classify_mode(const nrf_topology_snapshot_t *snapshot)
{
    nrf_topology_mode_info_t info = {.mode = NRF_TOPOLOGY_MODE_UNCERTAIN, .remote_count = 0U, .complete = false};
    if (snapshot == NULL) return info;

    uint8_t remotes = 0U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (snapshot->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) ++remotes;
    }

    if (snapshot->main.state == NRF_TOPOLOGY_NODE_ASSIGNED) {
        info.mode = NRF_TOPOLOGY_MODE_MAIN_PRESENT;
    } else if (snapshot->main.state == NRF_TOPOLOGY_NODE_ABSENT && snapshot->tca_available &&
               !snapshot->tca_fault && !snapshot->bus_busy && !snapshot->stale && remotes > 0U) {
        info.mode = NRF_TOPOLOGY_MODE_REMOTE_ONLY;
    } else {
        return info;
    }
    info.remote_count = remotes;
    info.complete = remotes == NRF_TOPOLOGY_FULL_REMOTE_COUNT;
    return info;
}

const char *nrf_topology_mode_name(nrf_topology_mode_t mode)
{
    switch (mode) {
    case NRF_TOPOLOGY_MODE_MAIN_PRESENT: return "MAIN_PRESENT";
    case NRF_TOPOLOGY_MODE_REMOTE_ONLY: return "REMOTE_ONLY";
    case NRF_TOPOLOGY_MODE_UNCERTAIN: break;
    }
    return "UNCERTAIN";
}

bool nrf_topology_select_status_node(
    bool main_assigned, nrf_cs_node_id_t main_node_id, const nrf_cs_node_id_t *nodes, size_t node_count,
    nrf_cs_node_id_t *status_node)
{
    if (nodes == NULL || node_count == 0U || status_node == NULL) return false;

    nrf_cs_node_id_t lowest = nodes[0];
    bool main_in_cycle = false;
    for (size_t i = 0U; i < node_count; ++i) {
        if (nodes[i] < lowest) lowest = nodes[i];
        if (nodes[i] == main_node_id) main_in_cycle = true;
    }
    *status_node = main_assigned && main_in_cycle ? main_node_id : lowest;
    return true;
}
