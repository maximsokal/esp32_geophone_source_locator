#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nrf_topology.h"
#include "nrf_upstream_protocol.h"

typedef enum {
    NRF_SENSOR_CACHE_EMPTY = 0,
    NRF_SENSOR_CACHE_PENDING,
    NRF_SENSOR_CACHE_OK,
    NRF_SENSOR_CACHE_PARTIAL,
    NRF_SENSOR_CACHE_NO_DATA,
    NRF_SENSOR_CACHE_SENSOR_BUSY,
    NRF_SENSOR_CACHE_SENSOR_ERROR,
    NRF_SENSOR_CACHE_TIMEOUT,
    NRF_SENSOR_CACHE_PROTOCOL_ERROR,
    NRF_SENSOR_CACHE_TRANSPORT_ERROR,
    NRF_SENSOR_CACHE_IDENTITY_STALE,
    NRF_SENSOR_CACHE_TOPOLOGY_STALE,
    NRF_SENSOR_CACHE_MUX_FAULT,
    NRF_SENSOR_CACHE_ABORTED,
} nrf_sensor_cache_state_t;

typedef struct {
    bool valid;
    bool identity_matches;
    nrf_topology_node_snapshot_t identity;
    uint8_t lifecycle;
    uint8_t result;
    uint8_t flags;
    uint64_t esp_receipt_uptime_ms;
} nrf_upstream_status_cache_t;

typedef struct {
    bool identity_valid;
    bool topology_current;
    bool response_valid;
    bool initial_refresh_issued;
    nrf_topology_node_snapshot_t identity;
    uint32_t topology_generation;
    uint32_t sensor_request_id;
    nrf_sensor_cache_state_t state;
    nrf_sensor_cache_state_t last_attempt_state;
    nrf_upstream_status_cache_t upstream_status;
    nrf_upstream_sensor_snapshot_t sensor;
    uint64_t esp_receipt_uptime_ms;
} nrf_remote_sensor_cache_t;
