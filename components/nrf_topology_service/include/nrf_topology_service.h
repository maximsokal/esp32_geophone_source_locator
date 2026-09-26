#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology_leveling_policy.h"
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

typedef enum {
    NRF_TOPOLOGY_STACK_PHASE_FIRST_RECONCILE = 0,
    NRF_TOPOLOGY_STACK_PHASE_RECONCILE,
    NRF_TOPOLOGY_STACK_PHASE_SENSOR_REFRESH,
    NRF_TOPOLOGY_STACK_PHASE_CS_CYCLE_START,
    NRF_TOPOLOGY_STACK_PHASE_CS_PUMP_PUBLISH,
    NRF_TOPOLOGY_STACK_PHASE_ERROR_RECOVERY,
    NRF_TOPOLOGY_STACK_PHASE_COUNT
} nrf_topology_stack_stats_phase_t;

typedef struct {
    uint32_t minimum_free_bytes;
    uint32_t phase_minimum_free_bytes[NRF_TOPOLOGY_STACK_PHASE_COUNT];
    uint8_t sampled_phase_mask;
} nrf_topology_service_stack_stats_t;

/*
 * Aggregate leveling status across every currently topology-assigned REMOTE
 * channel, for the acquisition/recording leveling gate. MAIN is excluded:
 * it has no external IMU/orientation assembly. A channel counts as
 * "accepted" only when its cached sensor response is both topology-current
 * and reports NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED — a stale or
 * not-yet-refreshed cache entry does not count.
 */
esp_err_t nrf_topology_service_start(void);
void nrf_topology_service_stop(void);
esp_err_t nrf_topology_service_request_refresh(void);
esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
esp_err_t nrf_topology_service_request_sensor_refresh(uint8_t channel);
esp_err_t nrf_topology_service_request_all_sensor_refresh(void);
/* Queues CAPTURE for all assigned REMOTEs; generations use 1..65535 and wrap to 1 (zero is never allocated). */
esp_err_t nrf_topology_service_request_height_reference_capture(uint8_t *requested_channel_mask);
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot);
esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary);
/* MAX-114: per-node levelling state of every assigned REMOTE. */
esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes);
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot);
esp_err_t nrf_topology_service_get_stack_stats(nrf_topology_service_stack_stats_t *stats);

#if defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
void nrf_topology_service_test_set_height_reference_generation(uint16_t generation);
/* Test-only state injection; never enabled by production builds. */
esp_err_t nrf_topology_service_test_publish_state(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT]);
#endif
