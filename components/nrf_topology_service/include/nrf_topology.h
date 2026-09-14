#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_upstream_protocol.h"

#define NRF_TOPOLOGY_REMOTE_COUNT 8U
#define NRF_TOPOLOGY_MAIN_NODE_ID 0U
#define NRF_TOPOLOGY_DEFAULT_CONFIRM_TIMEOUT_MS 500U
#define NRF_TOPOLOGY_DEFAULT_POLL_INTERVAL_MS 10U

typedef enum {
    NRF_TOPOLOGY_NODE_UNKNOWN = 0,
    NRF_TOPOLOGY_NODE_ABSENT,
    NRF_TOPOLOGY_NODE_ASSIGNED,
    NRF_TOPOLOGY_NODE_CONFLICT,
    NRF_TOPOLOGY_NODE_PROTOCOL_ERROR,
    NRF_TOPOLOGY_NODE_TRANSPORT_ERROR,
} nrf_topology_node_state_t;

typedef struct {
    nrf_topology_node_state_t state;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t node_id;
    uint8_t mux_channel;
    uint8_t current_address;
    uint8_t protocol_result;
    uint32_t assignment_request_id;
} nrf_topology_node_snapshot_t;

typedef struct {
    uint32_t generation;
    bool tca_available;
    bool tca_fault;
    nrf_topology_node_snapshot_t main;
    nrf_topology_node_snapshot_t remotes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_snapshot_t;

typedef struct {
    uint32_t confirm_timeout_ms;
    uint32_t poll_interval_ms;
    uint32_t request_seed;
} nrf_topology_config_t;

typedef struct {
    void *context;
    bool tca_available;
    bool tca_fault;
    esp_err_t (*mux_close)(void *context);
    esp_err_t (*mux_select)(void *context, uint8_t channel);
    esp_err_t (*read_status)(void *context, uint8_t address, nrf_upstream_status_t *status);
    esp_err_t (*write_assign)(void *context, uint8_t address, const nrf_upstream_assign_t *assign);
    void (*sleep_ms)(void *context, uint32_t delay_ms);
    bool (*abort_requested)(void *context);
} nrf_topology_io_t;

typedef struct {
    nrf_topology_config_t config;
    uint32_t next_request_id;
    uint32_t generation;
} nrf_topology_controller_t;

esp_err_t nrf_topology_controller_init(
    nrf_topology_controller_t *controller,
    const nrf_topology_config_t *config);

esp_err_t nrf_topology_reconcile(
    nrf_topology_controller_t *controller,
    const nrf_topology_io_t *io,
    nrf_topology_snapshot_t *snapshot);
