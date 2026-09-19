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

/* Pure aggregation used by the lock-protected topology service wrapper. */
esp_err_t nrf_topology_leveling_summary_compute(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_topology_leveling_summary_t *summary);
