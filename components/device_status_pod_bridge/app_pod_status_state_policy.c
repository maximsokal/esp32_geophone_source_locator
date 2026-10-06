#include "app_pod_status_state_policy.h"

pod_state_t app_pod_status_bridge_map_topology_state(
    nrf_topology_node_state_t state)
{
    switch (state) {
        case NRF_TOPOLOGY_NODE_ASSIGNED:
            return POD_STATE_READY;
        case NRF_TOPOLOGY_NODE_TRANSPORT_ERROR:
            return POD_STATE_WARNING;
        case NRF_TOPOLOGY_NODE_CONFLICT:
        case NRF_TOPOLOGY_NODE_PROTOCOL_ERROR:
            return POD_STATE_ERROR;
        case NRF_TOPOLOGY_NODE_UNKNOWN:
        case NRF_TOPOLOGY_NODE_ABSENT:
        default:
            return POD_STATE_DISCONNECTED;
    }
}
