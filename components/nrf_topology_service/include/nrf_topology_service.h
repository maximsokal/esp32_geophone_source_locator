#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_log_collector.h"
#include "nrf_node_link_status_policy.h"
#include "nrf_sensor_cache.h"
#include "nrf_topology_leveling_policy.h"
#include "nrf_topology.h"
#include "nrf_topology_mode.h"

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

/*
 * Heights of the REMOTE nodes relative to MAIN as the placement step settled them: the centre of its window,
 * good to about 0.1 m (workflow_core.h). The geometry of the ranging and of the recording takes Z from here, not
 * from the instantaneous barometer reading, which wanders by 10..20 cm (bench log 2026-11-01 18:16). The
 * workflow writes it while it tracks the placement and keeps it through ranging and recording; it is cleared
 * when the workflow goes back before the placement. A height only counts for the MAIN height-reference
 * generation it was measured against.
 */
typedef struct {
    bool valid;
    uint16_t generation;
    bool node_valid[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_cs_node_id_t node_id[NRF_TOPOLOGY_REMOTE_COUNT];
    int32_t height_rel_main_mm[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_placement_heights_t;

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
    bool success;
    bool membership_identity_valid;
    size_t membership_count;
    nrf_topology_ranging_member_identity_t membership[NRF_CS_MAX_NODES];
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    nrf_topology_ranging_geometry_t geometry;
    uint32_t topology_generation;
    uint32_t membership_generation;
    uint16_t main_height_reference_generation;
    /*
     * The topology the ranging ran on and the node that is the origin of its geometry: MAIN_PRESENT with MAIN's id, or
     * REMOTE_ONLY with the lowest REMOTE. NRF_TOPOLOGY_MODE_UNCERTAIN means "not recorded": a result older than these
     * fields, read by the legacy scheme (a membership entry without a mux channel is MAIN, so MAIN_PRESENT).
     */
    nrf_topology_mode_t topology_mode;
    nrf_cs_node_id_t anchor_node_id;
    uint64_t timestamp_ms;
    uint64_t accepted_revision_ms;
} nrf_topology_ranging_result_t;

/*
 * The longest distance an edge can show from the cables alone, for the bounds the triangles give a missing edge: a node
 * is at most its cable plus one measurement bin from MAIN, so a MAIN edge is that long and an edge between two devices
 * at most both together. -1 when a cable is unknown (limit 0) or both ends are MAIN.
 */
static inline int64_t nrf_topology_edge_cable_cap_mm(bool a_is_main, bool b_is_main, uint32_t limit_a_mm, uint32_t limit_b_mm)
{
    const int64_t bin = NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM;
    if (a_is_main && b_is_main) return -1;
    if (a_is_main) return limit_b_mm == 0U ? -1 : (int64_t)limit_b_mm + bin;
    if (b_is_main) return limit_a_mm == 0U ? -1 : (int64_t)limit_a_mm + bin;
    if (limit_a_mm == 0U || limit_b_mm == 0U) return -1;
    return (int64_t)limit_a_mm + (int64_t)limit_b_mm + 2 * bin;
}

/* Whether a distance is beyond a cable. An automatic one is quantized by a measurement bin and gets that much slack; a
 * manual one is exact and gets none. A negative distance never fits. */
static inline bool nrf_topology_pair_exceeds_cable(int32_t distance_mm, uint64_t limit_mm, uint32_t quality_flags)
{
    const uint64_t slack = (quality_flags & NRF_CS_QUALITY_MANUAL) != 0U ? 0U : NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM;
    return distance_mm < 0 || (uint64_t)distance_mm > limit_mm + slack;
}

/*
 * REMOTE_ONLY (no nRF MAIN): the housing is not a node, so a cable bounds a pair only through the two cables together. A pair
 * is beyond them when its distance exceeds both limits; an automatic one is quantized by a measurement bin and gets the
 * allowance of nrf_topology_edge_cable_cap_mm() (one bin per node), a manual one is exact. An unknown cable (limit 0) cannot
 * be checked and never exceeds.
 */
static inline bool nrf_topology_pair_exceeds_cable_sum(
    int32_t distance_mm, uint32_t limit_a_mm, uint32_t limit_b_mm, uint32_t quality_flags)
{
    if (limit_a_mm == 0U || limit_b_mm == 0U) return false;
    if (distance_mm < 0) return true;
    const uint64_t slack = (quality_flags & NRF_CS_QUALITY_MANUAL) != 0U ? 0U : 2U * NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM;
    return (uint64_t)distance_mm > (uint64_t)limit_a_mm + (uint64_t)limit_b_mm + slack;
}

/*
 * How far past its cable a fitted position may lie: one mm for the rounded XYZ, plus as much as the measured MAIN edge of
 * that node itself exceeded the cable (an automatic distance can, by less than one measurement bin, see
 * NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM). A node whose MAIN edge is within the cable, missing, or manual gets only the
 * rounding millimetre, so a fit that pushes a node outward against its measured distance is still rejected.
 */
static inline uint32_t nrf_topology_cable_radius_allowance_mm(
    const nrf_cs_pair_result_t *pairs, size_t pair_count, uint8_t main_node_id, uint8_t node_id, uint32_t limit_mm)
{
    uint32_t allowance = 1U;
    for (size_t i = 0U; pairs != NULL && i < pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = &pairs[i];
        const bool main_edge = (pair->node_a == main_node_id && pair->node_b == node_id) ||
            (pair->node_b == main_node_id && pair->node_a == node_id);
        if (!main_edge || pair->state != NRF_CS_PAIR_VALID || (pair->scheduler_quality_flags & NRF_CS_QUALITY_MANUAL) != 0U ||
            pair->distance_mm <= 0 || (uint32_t)pair->distance_mm <= limit_mm) {
            continue;
        }
        const uint32_t over = (uint32_t)pair->distance_mm - limit_mm;
        allowance += over < NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM ? over : NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM;
    }
    return allowance;
}

typedef struct {
    bool active;
    bool complete;
    bool success; /* All participating positions passed geometry acceptance. */
    bool partial; /* Retained measurements with incomplete coverage or geometry; may accompany success. */
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
    /*
     * The topology the ranging ran on and the node that is the origin of its geometry: MAIN_PRESENT with MAIN's id, or
     * REMOTE_ONLY with the lowest REMOTE. NRF_TOPOLOGY_MODE_UNCERTAIN means "not recorded": a result older than these
     * fields, read by the legacy scheme (a membership entry without a mux channel is MAIN, so MAIN_PRESENT).
     */
    nrf_topology_mode_t topology_mode;
    nrf_cs_node_id_t anchor_node_id;
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
/* The mode (MAIN_PRESENT, REMOTE_ONLY, UNCERTAIN) of the latest snapshot; ESP_ERR_INVALID_STATE before the first one. */
esp_err_t nrf_topology_service_get_mode(nrf_topology_mode_info_t *info);
esp_err_t nrf_topology_service_request_sensor_refresh(uint8_t channel);
esp_err_t nrf_topology_service_request_sensor_scan(uint8_t channel, uint32_t *request_id);
esp_err_t nrf_topology_service_get_sensor_scan_dispatch(uint32_t request_id, bool *pending, esp_err_t *result);
esp_err_t nrf_topology_service_request_all_sensor_refresh(void);
/* Queues CAPTURE for all assigned REMOTEs; generations use 1..65535 and wrap to 1 (zero is never allocated). */
esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation);
esp_err_t nrf_topology_service_request_imu_calibration(
    uint16_t generation,
    uint8_t channel_mask);
esp_err_t nrf_topology_service_get_sensor_snapshot(uint8_t channel, nrf_remote_sensor_cache_t *snapshot);
/* NULL clears the heights. */
esp_err_t nrf_topology_service_set_placement_heights(const nrf_topology_placement_heights_t *heights);
/* True and the height when the placement settled this node against this MAIN reference generation. */
bool nrf_topology_service_get_placement_height_mm(
    nrf_cs_node_id_t node_id, uint16_t generation, int32_t *height_mm);
/* node_slot 0 is MAIN; node_slots 1..8 are G1..G8 in mux-channel order. */
esp_err_t nrf_topology_service_get_node_status_snapshot(
    uint8_t node_slot,
    nrf_node_link_status_input_t *snapshot);
esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary);
/* MAX-114: per-node levelling state of every assigned REMOTE. */
esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes);
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot);
esp_err_t nrf_topology_service_ranging_start(
    const nrf_cs_policy_t *policy,
    uint32_t timeout_ms);
/*
 * Ranging for the workflow: the nodes range among themselves over Bluetooth while the shared I2C bus
 * stays silent (quiet lease); the worker is busy for the whole cycle and the progress only moves when
 * MAIN is polled. The time limit is derived from the policy and the node count.
 */
esp_err_t nrf_topology_service_ranging_start_autonomous(const nrf_cs_policy_t *policy);
/* Per mux channel; every assigned remote must have a nonzero limit in mm. */
esp_err_t nrf_topology_service_ranging_start_autonomous_limited(
    const nrf_cs_policy_t *policy, const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT]);
esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress);
/* No radio/I2C traffic; rejects stale revisions and changed node identities. */
esp_err_t nrf_topology_service_ranging_set_manual(
    uint64_t expected_started_at_ms, uint64_t expected_updated_at_ms,
    uint8_t node_a, uint8_t node_b, int32_t distance_mm,
    const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT]);
esp_err_t nrf_topology_service_get_frozen_ranging_result(
    nrf_topology_ranging_result_t *result);
/* Automatic VALID pairs require both directions; explicit manual corrections remain reusable. */
esp_err_t nrf_topology_service_get_reusable_frozen_ranging_result(
    const nrf_topology_snapshot_t *current_topology,
    uint64_t now_ms,
    uint64_t max_age_ms,
    nrf_topology_ranging_result_t *result);
/* Bind the current completed ranging revision to the operator-confirmed object. */
esp_err_t nrf_topology_service_accept_ranging_result(
    const char *object_id, const nrf_topology_snapshot_t *current_topology, uint64_t expected_updated_at_ms);
/*
 * ESP_OK: the confirmed result is valid for this object and membership. ESP_ERR_NOT_FOUND: it is gone or the setup
 * changed (membership, object, invalidated). ESP_ERR_TIMEOUT: it is still the confirmed one but the topology
 * snapshot is stale (bus error); try again, do not treat it as a change.
 */
esp_err_t nrf_topology_service_get_accepted_ranging_result(
    const char *object_id, const nrf_topology_snapshot_t *current_topology, uint64_t now_ms,
    nrf_topology_ranging_result_t *result);
esp_err_t nrf_topology_service_invalidate_ranging(void);
/* Abort the running or queued ranging cycle. Best effort and idempotent: ESP_OK when nothing runs. */
esp_err_t nrf_topology_service_ranging_cancel(void);
/*
 * Shutdown: cancel like nrf_topology_service_ranging_cancel() and refuse every new Channel
 * Sounding request until the service is started again, so a START that is still on its way to
 * Channel Sounding cannot begin a cycle the shutdown would have to wait for.
 */
esp_err_t nrf_topology_service_ranging_disable(void);
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
    nrf_upstream_status_t *status,
    bool after_log);
esp_err_t nrf_topology_service_test_direct_nrf_io(
    uint8_t address,
    bool write);
/* Test-only state injection; never enabled by production builds. */
esp_err_t nrf_topology_service_test_publish_state(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT]);
/* Test-only: replaces the frozen ranging result (a legacy or a deliberately inconsistent one). */
esp_err_t nrf_topology_service_test_set_frozen_result(const nrf_topology_ranging_result_t *result);
#endif
