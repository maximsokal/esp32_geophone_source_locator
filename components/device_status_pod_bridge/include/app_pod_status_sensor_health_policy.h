#pragma once

#include "device_status.h"
#include "nrf_sensor_cache.h"

device_sensor_health_t app_pod_status_sensor_health(
    const nrf_remote_sensor_cache_t *cache, uint64_t now_ms);
