#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NRF_TOPOLOGY_REMOTE_COUNT 8U
#define NRF_TOPOLOGY_MAIN_NODE_ID 0U
#define NRF_CS_MAX_PAIRS 10U
#define NRF_UPSTREAM_SENSOR_VALID_ENV 0x02U
#define NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED 2U
#define NRF_TOPOLOGY_RANGING_DEFAULT_TIMEOUT_MS 30000U
#define NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE 48U

typedef enum {
    NRF_TOPOLOGY_NODE_UNKNOWN = 0,
    NRF_TOPOLOGY_NODE_ABSENT,
    NRF_TOPOLOGY_NODE_ASSIGNED,
} nrf_topology_node_state_t;

typedef struct {
    nrf_topology_node_state_t state;
    uint8_t node_id;
} nrf_topology_node_snapshot_t;
typedef nrf_topology_node_snapshot_t nrf_topology_remote_snapshot_t;

typedef struct {
    uint32_t generation;
    nrf_topology_node_snapshot_t main;
    nrf_topology_node_snapshot_t remotes[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_snapshot_t;

typedef struct {
    bool identity_valid;
    bool topology_current;
    bool response_valid;
    struct { uint8_t node_id; } identity;
    struct {
        uint32_t valid_mask;
        uint32_t pressure_pa;
        int32_t altitude_mm;
        uint8_t orientation_state;
        uint8_t reference_state;
        uint16_t reference_generation;
        int32_t relative_z_mm;
        uint8_t imu_cal_state;
        uint16_t imu_cal_generation;
        int16_t gyro_bias_mdps[3];
        int16_t gravity_ref_mg[3];
    } sensor;
    uint64_t esp_receipt_uptime_ms;
} nrf_remote_sensor_cache_t;

typedef enum {
    NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED = 0,
    NRF_UPSTREAM_REFERENCE_VALID,
    NRF_UPSTREAM_REFERENCE_REJECTED_MOVING,
    NRF_UPSTREAM_REFERENCE_SENSOR_INVALID,
} nrf_upstream_reference_state_t;

typedef enum {
    NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED = 0,
    NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED,
    NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING,
    NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY,
    NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID,
} nrf_upstream_imu_calibration_state_t;

typedef enum {
    NRF_CS_PAIR_UNKNOWN = 0,
    NRF_CS_PAIR_PENDING,
    NRF_CS_PAIR_VALID,
    NRF_CS_PAIR_MISSING,
    NRF_CS_PAIR_RETRY_EXHAUSTED,
    NRF_CS_PAIR_CABLE_EXCEEDED,
    NRF_CS_PAIR_DIRECTIONS_DISAGREE,
} nrf_cs_pair_state_t;

typedef struct {
    uint8_t node_a;
    uint8_t node_b;
    nrf_cs_pair_state_t state;
    int32_t distance_mm;
    uint16_t confidence, attempts;
    uint8_t valid_samples;
    int32_t spread_mm;
    uint8_t rejected_samples;
    uint16_t scheduler_quality_flags;
    bool single_direction;
    int32_t direction_disagreement_mm;
    int32_t automatic_distance_mm;
} nrf_cs_pair_result_t;

typedef struct {
    bool active;
    bool complete;
    int result;
    uint32_t cycle_id;
    uint32_t membership_generation;
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
} nrf_cs_cycle_snapshot_t;

typedef struct {
    uint32_t pair_timeout_ms;
    uint8_t max_attempts;
    uint16_t min_confidence;
    uint8_t samples_per_pair;
    bool full_rotation;
    bool relay_order;
} nrf_cs_policy_t;

#define NRF_CS_MAX_NODES 5U
typedef uint8_t nrf_cs_node_id_t;

typedef struct {
    nrf_cs_node_id_t node_id;
    bool solved;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    int32_t residual_mm;
    int32_t uncertainty_mm;
} nrf_topology_ranging_node_t;

typedef struct {
    size_t node_count;
    nrf_topology_ranging_node_t nodes[NRF_CS_MAX_NODES];
    int32_t max_residual_mm;
    size_t inconsistent_pair_count;
} nrf_topology_ranging_geometry_t;

typedef struct {
    bool active;
    bool complete;
    bool success;
    bool partial;
    size_t pairs_done;
    size_t pairs_total;
    size_t pair_count;
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    nrf_topology_ranging_geometry_t geometry;
    int32_t solution_residual_mm;
    char failure_reason[NRF_TOPOLOGY_RANGING_FAILURE_REASON_SIZE];
    uint16_t main_height_reference_generation;
    uint64_t started_at_ms;
    uint64_t updated_at_ms;
} nrf_topology_ranging_progress_t;

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot);
esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation);
esp_err_t nrf_topology_service_request_imu_calibration(uint16_t generation, uint8_t channel_mask);
esp_err_t nrf_topology_service_get_sensor_snapshot(
    uint8_t channel,
    nrf_remote_sensor_cache_t *snapshot);
esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot);
esp_err_t nrf_topology_service_ranging_start_autonomous(const nrf_cs_policy_t *policy);
esp_err_t nrf_topology_service_ranging_start_autonomous_limited(const nrf_cs_policy_t *policy, const uint32_t *limits);
esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress);
esp_err_t nrf_topology_service_invalidate_ranging(void);
esp_err_t nrf_topology_service_ranging_cancel(void);
esp_err_t nrf_topology_service_ranging_set_manual(uint64_t started, uint64_t updated,
    uint8_t a, uint8_t b, int32_t distance, const uint32_t *limits);

typedef struct {
    bool valid;
    uint16_t generation;
    bool node_valid[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_cs_node_id_t node_id[NRF_TOPOLOGY_REMOTE_COUNT];
    int32_t height_rel_main_mm[NRF_TOPOLOGY_REMOTE_COUNT];
} nrf_topology_placement_heights_t;
esp_err_t nrf_topology_service_set_placement_heights(const nrf_topology_placement_heights_t *heights);
