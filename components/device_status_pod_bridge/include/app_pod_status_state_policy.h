#pragma once

#include "device_status.h"
#include "nrf_topology.h"

/* Pure mapping from topology discovery state to the pod connection state. */
pod_state_t app_pod_status_bridge_map_topology_state(
    nrf_topology_node_state_t state);
