#pragma once

#include "esp_err.h"
#include "nrf_topology.h"

esp_err_t nrf_topology_service_start(void);
void nrf_topology_service_stop(void);
esp_err_t nrf_topology_service_request_refresh(void);
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
