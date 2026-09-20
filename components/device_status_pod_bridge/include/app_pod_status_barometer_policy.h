#pragma once

#include <stdbool.h>

#include "nrf_sensor_cache.h"

typedef struct {
    float pressure_pa;
    float relative_altitude_cm;
    bool valid;
} app_pod_status_barometer_reading_t;

/*
 * Pure mapping from one nRF topology sensor-cache entry to what the pod
 * status bridge publishes through device_status_set_pod_barometer(). Valid
 * only when the cache entry is a real, current response (not stale/pending)
 * and its ENV domain (pressure/altitude) is itself flagged valid; otherwise
 * returns a zeroed, invalid reading rather than stale or fabricated numbers.
 */
app_pod_status_barometer_reading_t app_pod_status_bridge_compute_barometer_reading(
    const nrf_remote_sensor_cache_t *cache);
