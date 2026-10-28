#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology.h"
#include "nrf_upstream_protocol.h"

#define NRF_SENSOR_TRANSPORT_DEFAULT_TIMEOUT_MS 500U
#define NRF_SENSOR_TRANSPORT_DEFAULT_POLL_MS 10U

typedef struct {
    uint32_t timeout_ms;
    uint32_t poll_interval_ms;
    uint32_t request_seed;
} nrf_sensor_transport_config_t;

typedef struct {
    nrf_sensor_transport_config_t config;
    uint32_t next_request_id;
    nrf_upstream_status_cache_t main_status_cache;
} nrf_sensor_transport_controller_t;

typedef struct {
    void *context;
    esp_err_t (*mux_select)(void *context, uint8_t channel);
    esp_err_t (*mux_close)(void *context);
    bool (*mux_faulted)(void *context);
    esp_err_t (*read_status)(void *context, uint8_t address, nrf_upstream_status_t *status);
    esp_err_t (*write_sensor_refresh)(void *context, const nrf_upstream_sensor_refresh_t *request);
    esp_err_t (*write_height_reference)(void *context, const nrf_upstream_height_reference_t *request);
    esp_err_t (*write_imu_calibration)(void *context, const nrf_upstream_imu_calibrate_t *request);
    esp_err_t (*write_sensor_scan)(void *context, const nrf_upstream_sensor_refresh_t *request);
    esp_err_t (*read_remote_snapshot)(void *context, nrf_upstream_status_t *status, nrf_upstream_sensor_snapshot_t *sensor);
    void (*sleep_ms)(void *context, uint32_t delay_ms);
    uint64_t (*now_ms)(void *context);
    bool (*abort_requested)(void *context);
} nrf_sensor_transport_io_t;

esp_err_t nrf_sensor_transport_controller_init(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_config_t *config);

uint8_t nrf_sensor_cache_reconcile_topology(
    const nrf_topology_snapshot_t *topology,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT]);

esp_err_t nrf_sensor_transport_refresh_channel(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    const nrf_topology_snapshot_t *topology,
    uint8_t channel,
    nrf_remote_sensor_cache_t *cache,
    bool *request_topology_reconcile);

esp_err_t nrf_sensor_transport_capture_height_reference(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    const nrf_topology_snapshot_t *topology,
    uint8_t channel,
    uint16_t generation,
    bool *request_topology_reconcile);

esp_err_t nrf_sensor_transport_request_imu_calibration(
    nrf_sensor_transport_controller_t *controller,
    const nrf_sensor_transport_io_t *io,
    const nrf_topology_snapshot_t *topology,
    uint8_t channel,
    uint16_t generation,
    bool *request_topology_reconcile);

esp_err_t nrf_sensor_transport_request_scan(
    const nrf_sensor_transport_io_t *io, const nrf_topology_snapshot_t *topology,
    uint8_t channel, uint32_t request_id, bool *request_topology_reconcile);
