#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    size_t assigned_count;
    size_t accepted_count;
} nrf_topology_leveling_summary_t;

esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary
);

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NRF_TOPOLOGY_LEVELING_NO_DATA = 0,
    NRF_TOPOLOGY_LEVELING_UNAVAILABLE,
    NRF_TOPOLOGY_LEVELING_MOVING,
    NRF_TOPOLOGY_LEVELING_ACCEPTED,
    NRF_TOPOLOGY_LEVELING_OUTSIDE,
} nrf_topology_leveling_node_state_t;

typedef struct {
    uint8_t channel;
    uint8_t node_id;
    nrf_topology_leveling_node_state_t state;
    bool tilt_valid;
    float tilt_deg;
} nrf_topology_leveling_node_t;

typedef struct {
    size_t count;
    nrf_topology_leveling_node_t nodes[8];
} nrf_topology_leveling_nodes_t;

esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes);
const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state);
