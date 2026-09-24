#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology.h"

typedef struct {
    size_t assigned_count;
    size_t accepted_count;
} nrf_topology_leveling_summary_t;

/* MAX-114: per-node levelling state for the UI (why recording is withheld). */
typedef enum {
    /* Assigned, but no current sensor response yet. */
    NRF_TOPOLOGY_LEVELING_NO_DATA = 0,
    /* Values below mirror nrf_orientation_state_t (wire value + 1). */
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
    nrf_topology_leveling_node_t nodes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_leveling_nodes_t;

/* Pure aggregation used by the lock-protected topology service wrapper. */
esp_err_t nrf_topology_leveling_summary_compute(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_topology_leveling_summary_t *summary);

/* One entry per ASSIGNED REMOTE channel, in channel order. */
esp_err_t nrf_topology_leveling_nodes_compute(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_topology_leveling_nodes_t *nodes);

/* Tilt from vertical, degrees, from the nRF's sin^2(tilt) in ppm. */
float nrf_topology_leveling_tilt_deg(uint32_t tilt_sin2_ppm);

const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state);
