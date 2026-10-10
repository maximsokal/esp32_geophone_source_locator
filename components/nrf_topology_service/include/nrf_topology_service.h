#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_log_collector.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology_leveling_policy.h"
#include "nrf_topology.h"

typedef struct {
    bool active;
    bool complete;
    int result;
    uint32_t cycle_id;
    uint32_t membership_generation;
    size_t executions_done;
    size_t executions_total;
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
} nrf_cs_cycle_snapshot_t;

#define NRF_TOPOLOGY_RANGING_DEFAULT_TIMEOUT_MS 30000U
#define NRF_TOPOLOGY_RANGING_DEFAULT_REUSE_MAX_AGE_MS 600000U
#define NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE 48U

typedef struct {
    nrf_cs_node_id_t node_id;
    bool solved;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    int32_t residual_mm;
    /* 1-sigma mm; INT32_MAX means the node is unsolved/unbounded. */
    int32_t uncertainty_mm;
} nrf_topology_ranging_node_t;

typedef struct {
    uint8_t channel;
    nrf_cs_node_id_t node_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
} nrf_topology_ranging_member_identity_t;

typedef struct {
    size_t node_count;
    nrf_topology_ranging_node_t nodes[NRF_CS_MAX_NODES];
    int32_t max_residual_mm;
    size_t inconsistent_pair_count;
    bool refined;
    uint8_t refine_iterations;
} nrf_topology_ranging_geometry_t;

typedef struct {
    bool available;
    bool invalidated;
    bool membership_identity_valid;
    size_t membership_count;
    nrf_topology_ranging_member_identity_t membership[NRF_CS_MAX_NODES];
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    nrf_topology_ranging_geometry_t geometry;
    uint32_t topology_generation;
    uint32_t membership_generation;
    uint16_t main_height_reference_generation;
    uint64_t timestamp_ms;
} nrf_topology_ranging_result_t;

typedef struct {
    bool active;
    bool complete;
    bool success;
    size_t pairs_done; /* Directed executions, despite the legacy field name. */
    size_t pairs_total;
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    nrf_topology_ranging_geometry_t geometry;
    int32_t solution_residual_mm;
    char failure_reason[NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE];
    uint32_t topology_generation;
    uint32_t membership_generation;
    uint16_t main_height_reference_generation;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
} nrf_topology_ranging_progress_t;

typedef enum {
    NRF_TOPOLOGY_STACK_PHASE_FIRST_RECONCILE = 0,
    NRF_TOPOLOGY_STACK_PHASE_RECONCILE,
    NRF_TOPOLOGY_STACK_PHASE_SENSOR_REFRESH,
    NRF_TOPOLOGY_STACK_PHASE_CS_CYCLE_START,
    NRF_TOPOLOGY_STACK_PHASE_CS_PUMP_PUBLISH,
    NRF_TOPOLOGY_STACK_PHASE_LOG_COLLECTOR,
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
esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation);
esp_err_t nrf_topology_service_request_imu_calibration(
    uint16_t generation,
    uint8_t channel_mask);
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot);
esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary);
/* MAX-114: per-node levelling state of every assigned REMOTE. */
esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes);
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot);
esp_err_t nrf_topology_service_ranging_start(
    const nrf_cs_policy_t *policy,
    uint32_t timeout_ms);
esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress);
esp_err_t nrf_topology_service_get_frozen_ranging_result(
    nrf_topology_ranging_result_t *result);
esp_err_t nrf_topology_service_get_reusable_frozen_ranging_result(
    const nrf_topology_snapshot_t *current_topology,
    uint64_t now_ms,
    uint64_t max_age_ms,
    nrf_topology_ranging_result_t *result);
esp_err_t nrf_topology_service_invalidate_ranging(void);
esp_err_t nrf_topology_service_get_stack_stats(nrf_topology_service_stack_stats_t *stats);
esp_err_t nrf_topology_service_get_nrf_log_summary(
    uint8_t node_index,
    nrf_log_node_summary_t *summary);
esp_err_t nrf_topology_service_get_nrf_log_page(
    uint8_t node_index,
    bool has_after,
    uint32_t after_boot_id,
    uint32_t after_seq,
    uint16_t limit,
    nrf_log_node_page_t *page);

#if defined(NRF_TOPOLOGY_SERVICE_TEST_HARNESS)
void nrf_topology_service_test_set_height_reference_generation(uint16_t generation);
esp_err_t nrf_topology_service_test_read_status_cached_probe(
    uint8_t address,
    nrf_upstream_status_t *status);
/* Test-only state injection; never enabled by production builds. */
esp_err_t nrf_topology_service_test_publish_state(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT]);
#endif
