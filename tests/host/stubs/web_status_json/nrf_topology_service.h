#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NRF_TOPOLOGY_REMOTE_COUNT 8U
#define NRF_LOG_COLLECTOR_NODE_COUNT 9U

typedef enum {
    NRF_TOPOLOGY_NODE_UNKNOWN = 0,
    NRF_TOPOLOGY_NODE_ABSENT,
    NRF_TOPOLOGY_NODE_ASSIGNED,
} nrf_topology_node_state_t;

typedef struct {
    nrf_topology_node_state_t state;
} nrf_topology_test_node_t;

typedef struct {
    nrf_topology_test_node_t main;
    nrf_topology_test_node_t remotes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_snapshot_t;

typedef struct {
    bool present;
    bool logs_unsupported;
    bool boot_id_valid;
    uint32_t boot_id;
    uint16_t stored;
    uint16_t warn_count;
    uint16_t error_count;
    uint16_t fatal_count;
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
    uint32_t wire_gap_total;
    uint32_t local_drop_total;
} nrf_log_node_summary_t;

int32_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
int32_t nrf_topology_service_get_nrf_log_summary(
    uint8_t node_index,
    nrf_log_node_summary_t *summary);
