#pragma once

#include "esp_err.h"

#define NRF_TOPOLOGY_REMOTE_COUNT 8U

typedef enum {
    NRF_TOPOLOGY_NODE_UNKNOWN = 0,
    NRF_TOPOLOGY_NODE_ABSENT,
    NRF_TOPOLOGY_NODE_ASSIGNED
} nrf_topology_node_state_t;

typedef struct {
    nrf_topology_node_state_t state;
} nrf_topology_remote_snapshot_t;

typedef struct {
    nrf_topology_remote_snapshot_t remotes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_snapshot_t;

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
