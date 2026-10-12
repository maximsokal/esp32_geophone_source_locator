#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nrf_topology_periods.h"

#define NRF_NODE_LINK_STATUS_STALE_PERIODS 3U
#define NRF_NODE_LINK_STATUS_STALE_LIMIT_MS \
    (NRF_NODE_LINK_STATUS_STALE_PERIODS * NRF_SENSOR_REFRESH_PERIOD_MS)

typedef enum {
    NRF_NODE_LINK_STATUS_NOT_CONNECTED = 0,
    NRF_NODE_LINK_STATUS_CONNECTED,
    NRF_NODE_LINK_STATUS_READY,
    NRF_NODE_LINK_STATUS_MEASURING,
} nrf_node_link_status_t;

typedef struct {
    bool assigned;
    bool status_valid;
    bool status_identity_matches;
    uint8_t lifecycle;
    uint8_t result;
    uint8_t flags;
    uint64_t status_receipt_uptime_ms;
    bool sensor_result_valid;
    uint8_t sensor_result;
} nrf_node_link_status_input_t;

nrf_node_link_status_t nrf_node_link_status_evaluate(
    const nrf_node_link_status_input_t *input,
    uint64_t now_ms,
    bool recording);

const char *nrf_node_link_status_name(nrf_node_link_status_t status);
