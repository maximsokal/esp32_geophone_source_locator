#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology.h"

esp_err_t nrf_topology_service_start(void);
void nrf_topology_service_stop(void);
esp_err_t nrf_topology_service_request_refresh(void);
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
esp_err_t nrf_topology_service_request_sensor_refresh(uint8_t channel);
esp_err_t nrf_topology_service_request_all_sensor_refresh(void);
esp_err_t nrf_topology_service_get_sensor_snapshot(
    uint8_t channel,
    nrf_remote_sensor_cache_t *snapshot);
