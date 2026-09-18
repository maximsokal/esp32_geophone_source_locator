#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology.h"

typedef struct {
    bool active;
    bool complete;
    int result;
    uint32_t cycle_id;
    uint32_t membership_generation;
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
} nrf_cs_cycle_snapshot_t;

/*
 * Aggregate leveling status across every currently topology-assigned REMOTE
 * channel, for the acquisition/recording leveling gate. MAIN is excluded:
 * it has no external IMU/orientation assembly. A channel counts as
 * "accepted" only when its cached sensor response is both topology-current
 * and reports NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED — a stale or
 * not-yet-refreshed cache entry does not count.
 */
typedef struct {
    size_t assigned_count;
    size_t accepted_count;
} nrf_topology_leveling_summary_t;

esp_err_t nrf_topology_service_start(void);
void nrf_topology_service_stop(void);
esp_err_t nrf_topology_service_request_refresh(void);
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
esp_err_t nrf_topology_service_request_sensor_refresh(uint8_t channel);
esp_err_t nrf_topology_service_request_all_sensor_refresh(void);
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot);
esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary);
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot);
