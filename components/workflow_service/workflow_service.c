#include "workflow_service.h"

#include <stdio.h>
#include <string.h>

#include "device_status.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "main_barometer.h"
#include "measurement_service.h"
#include "nrf_baro_altitude.h"
#include "nrf_topology_service.h"
#include "object_service.h"

#define WORKFLOW_CALIBRATION_TIMEOUT_MS 15000U
#define WORKFLOW_CALIBRATION_CS_MAX_SPREAD_MM 150
#define WORKFLOW_PLACEMENT_HISTORY_MAX_SAMPLES 32U

static const char *TAG = "WORKFLOW";

typedef struct {
    workflow_calibration_snapshot_t status;
    bool active;
    uint8_t remote_count;
    uint8_t channels[WORKFLOW_MAX_REMOTE_NODES];
    uint8_t node_ids[WORKFLOW_MAX_REMOTE_NODES];
    bool cs_pair_terminal[WORKFLOW_MAX_REMOTE_NODES];
    uint64_t started_ms;
    bool previous_cycle_valid;
    uint32_t previous_cycle_id;
    bool cycle_seen;
    uint32_t cycle_id;
} workflow_calibration_run_t;

typedef struct {
    uint64_t timestamp_ms;
    int64_t height_rel_main_mm;
} workflow_placement_sample_t;

typedef struct {
    bool node_valid;
    uint8_t node_id;
    bool vertical_known;
    bool vertical;
    uint16_t generation;
    uint64_t last_sample_ms;
    bool last_sample_valid;
    uint64_t blocked_until_ms;
    size_t sample_count;
    workflow_placement_sample_t samples[WORKFLOW_PLACEMENT_HISTORY_MAX_SAMPLES];
} workflow_placement_history_t;

typedef struct {
    workflow_placement_snapshot_t status;
    workflow_placement_history_t history[WORKFLOW_MAX_CALIBRATION_NODES];
} workflow_placement_run_t;

typedef struct {
    bool active;
    bool orphaned;
    uint64_t started_at_ms;
} workflow_ranging_run_t;

static SemaphoreHandle_t s_mutex;
static bool s_initialized;
static bool s_recorder_reconciliation_armed;
static uint8_t s_assigned_remotes;
static char s_active_object_id[OBJECT_SERVICE_OBJECT_ID_SIZE];
static workflow_core_t s_core;
static workflow_service_placement_ready_fn s_placement_ready_predicate;
static void *s_placement_ready_context;
static EXT_RAM_BSS_ATTR workflow_calibration_run_t s_calibration;
static EXT_RAM_BSS_ATTR workflow_placement_run_t s_placement;
static EXT_RAM_BSS_ATTR workflow_ranging_run_t s_ranging;

static const nrf_cs_policy_t s_workflow_ranging_policy = {
    .pair_timeout_ms = 500U,
    .max_attempts = 3U,
    .min_confidence = 500U,
    .samples_per_pair = 3U,
    .full_rotation = true,
};

static bool workflow_service_lock(void)
{
    return s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void workflow_service_unlock(void)
{
    (void)xSemaphoreGive(s_mutex);
}

static bool workflow_service_placement_ready(void)
{
    return s_placement.status.all_stable &&
        (s_placement_ready_predicate == NULL ||
         s_placement_ready_predicate(s_placement_ready_context));
}

static void workflow_service_copy_string(char *target, size_t capacity, const char *source)
{
    if (target == NULL || capacity == 0U) return;
    (void)snprintf(target, capacity, "%s", source != NULL ? source : "");
}

static uint64_t workflow_service_now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)now_us / UINT64_C(1000) : 0U;
}

static void workflow_service_calibration_add_reason(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') return;
    workflow_calibration_snapshot_t *status = &s_calibration.status;
    for (size_t i = 0U; i < status->reasons_count; ++i) {
        if (strcmp(status->reasons[i], reason) == 0) return;
    }
    if (status->reasons_count >= WORKFLOW_MAX_CALIBRATION_REASONS) return;
    workflow_service_copy_string(
        status->reasons[status->reasons_count++],
        WORKFLOW_CALIBRATION_REASON_MAX,
        reason);
}

static void workflow_service_calibration_log_node(
    const workflow_calibration_node_t *node)
{
    ESP_LOGI(
        TAG,
        "WORKFLOW_CAL node=%s height=%s imu=%s cs_pairs_ok=%s",
        node->node,
        node->height_state,
        node->imu_state,
        node->cs_pairs_ok ? "true" : "false");
}

static const char *workflow_service_height_failure(const char *state)
{
    if (strcmp(state, "rejected_moving") == 0) return "height_rejected_moving";
    if (strcmp(state, "sensor_invalid") == 0) return "height_sensor_invalid";
    return NULL;
}

static const char *workflow_service_imu_failure(const char *state)
{
    if (strcmp(state, "rejected_moving") == 0) return "imu_rejected_moving";
    if (strcmp(state, "rejected_gravity") == 0) return "imu_rejected_gravity";
    if (strcmp(state, "sensor_invalid") == 0) return "imu_sensor_invalid";
    return NULL;
}

static void workflow_service_add_node_reason(
    const workflow_calibration_node_t *node,
    const char *reason)
{
    if (reason == NULL) return;
    char full_reason[WORKFLOW_CALIBRATION_REASON_MAX];
    (void)snprintf(full_reason, sizeof(full_reason), "%s:%s", node->node, reason);
    workflow_service_calibration_add_reason(full_reason);
}

/* Shared by the HTTP control and the guided calibration run. */
esp_err_t workflow_service_capture_height_references(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation)
{
    if (requested_channel_mask == NULL || requested_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = nrf_topology_service_request_height_reference_capture(
        requested_channel_mask,
        requested_generation);
    if (err != ESP_OK) return err;
    (void)main_barometer_request_height_reference_capture(*requested_generation);
    return ESP_OK;
}

static void workflow_service_calibration_start_name(
    workflow_calibration_node_t *node,
    uint8_t node_id)
{
    if (node_id == NRF_TOPOLOGY_MAIN_NODE_ID) {
        workflow_service_copy_string(node->node, sizeof(node->node), "MAIN");
    } else {
        (void)snprintf(node->node, sizeof(node->node), "G%u", (unsigned)node_id);
    }
    workflow_service_copy_string(node->height_state, sizeof(node->height_state), "pending");
    workflow_service_copy_string(
        node->imu_state,
        sizeof(node->imu_state),
        node_id == NRF_TOPOLOGY_MAIN_NODE_ID ? "not_applicable" : "pending");
}

static esp_err_t workflow_service_calibration_start(void)
{
    nrf_topology_snapshot_t topology;
    esp_err_t err = nrf_topology_service_get_snapshot(&topology);
    if (err != ESP_OK) return err;

    uint8_t channel_mask = 0U;
    uint16_t generation = 0U;
    err = workflow_service_capture_height_references(&channel_mask, &generation);
    if (err != ESP_OK) return err;

    uint8_t channels[WORKFLOW_MAX_REMOTE_NODES];
    uint8_t node_ids[WORKFLOW_MAX_REMOTE_NODES];
    size_t remote_count = 0U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if ((channel_mask & (uint8_t)(1U << channel)) == 0U) continue;
        if (topology.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED ||
            topology.remotes[channel].node_id == NRF_TOPOLOGY_MAIN_NODE_ID ||
            remote_count >= WORKFLOW_MAX_REMOTE_NODES) {
            return ESP_ERR_INVALID_STATE;
        }
        channels[remote_count] = channel;
        node_ids[remote_count] = topology.remotes[channel].node_id;
        ++remote_count;
    }
    if (remote_count == 0U) return ESP_ERR_NOT_FOUND;

    err = nrf_topology_service_request_imu_calibration(generation, channel_mask);
    if (err != ESP_OK) return err;

    nrf_cs_cycle_snapshot_t previous_cycle;
    const bool previous_cycle_valid =
        nrf_topology_service_get_cs_cycle(&previous_cycle) == ESP_OK;
    const nrf_cs_policy_t policy = {
        .pair_timeout_ms = 500U,
        .max_attempts = 3U,
        .min_confidence = 500U,
        .samples_per_pair = 3U,
    };
    err = nrf_topology_service_request_cs_cycle(&policy);
    if (err != ESP_OK) return err;

    memset(&s_calibration, 0, sizeof(s_calibration));
    s_calibration.active = true;
    s_calibration.remote_count = (uint8_t)remote_count;
    memcpy(s_calibration.channels, channels, remote_count * sizeof(channels[0]));
    memcpy(s_calibration.node_ids, node_ids, remote_count * sizeof(node_ids[0]));
    s_calibration.started_ms = workflow_service_now_ms();
    s_calibration.previous_cycle_valid = previous_cycle_valid;
    if (previous_cycle_valid) s_calibration.previous_cycle_id = previous_cycle.cycle_id;
    s_calibration.status.generation = generation;
    workflow_service_copy_string(
        s_calibration.status.phase,
        sizeof(s_calibration.status.phase),
        "running");
    s_calibration.status.per_node_count = remote_count + 1U;
    s_calibration.status.pairs_total = remote_count;
    workflow_service_calibration_start_name(
        &s_calibration.status.per_node[0], NRF_TOPOLOGY_MAIN_NODE_ID);
    for (size_t i = 0U; i < remote_count; ++i) {
        workflow_service_calibration_start_name(
            &s_calibration.status.per_node[i + 1U], s_calibration.node_ids[i]);
        workflow_service_calibration_log_node(&s_calibration.status.per_node[i + 1U]);
    }
    workflow_service_calibration_log_node(&s_calibration.status.per_node[0]);
    return ESP_OK;
}

static void workflow_service_calibration_update_main_height(void)
{
    device_status_snapshot_t device;
    if (device_status_try_get_snapshot(&device) != ESP_OK) return;
    workflow_calibration_node_t *node = &s_calibration.status.per_node[0];
    if (device.main.height_reference.generation != s_calibration.status.generation) return;

    switch (device.main.height_reference.state) {
        case DEVICE_MAIN_HEIGHT_REFERENCE_VALID:
            workflow_service_copy_string(node->height_state, sizeof(node->height_state), "valid");
            node->relative_z_mm_valid = true;
            node->relative_z_mm = device.main.height_reference.relative_z_mm;
            break;
        case DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE:
            workflow_service_copy_string(
                node->height_state, sizeof(node->height_state), "rejected_moving");
            node->relative_z_mm_valid = false;
            break;
        case DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID:
            workflow_service_copy_string(
                node->height_state, sizeof(node->height_state), "sensor_invalid");
            node->relative_z_mm_valid = false;
            break;
        case DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED:
        default:
            break;
    }
    workflow_service_add_node_reason(node, workflow_service_height_failure(node->height_state));
}

static void workflow_service_calibration_update_remote(
    size_t index,
    nrf_remote_sensor_cache_t *cache)
{
    const uint8_t channel = s_calibration.channels[index];
    const uint8_t node_id = s_calibration.node_ids[index];
    workflow_calibration_node_t *node = &s_calibration.status.per_node[index + 1U];
    if (nrf_topology_service_get_sensor_snapshot(channel, cache) != ESP_OK ||
        !cache->identity_valid || !cache->topology_current || !cache->response_valid ||
        cache->identity.node_id != node_id) {
        return;
    }

    if (cache->sensor.reference_generation == s_calibration.status.generation) {
        switch (cache->sensor.reference_state) {
            case NRF_UPSTREAM_REFERENCE_VALID:
                workflow_service_copy_string(
                    node->height_state, sizeof(node->height_state), "valid");
                node->relative_z_mm_valid = true;
                node->relative_z_mm = cache->sensor.relative_z_mm;
                break;
            case NRF_UPSTREAM_REFERENCE_REJECTED_MOVING:
                workflow_service_copy_string(
                    node->height_state, sizeof(node->height_state), "rejected_moving");
                node->relative_z_mm_valid = false;
                break;
            case NRF_UPSTREAM_REFERENCE_SENSOR_INVALID:
                workflow_service_copy_string(
                    node->height_state, sizeof(node->height_state), "sensor_invalid");
                node->relative_z_mm_valid = false;
                break;
            case NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED:
            default:
                break;
        }
    }
    workflow_service_add_node_reason(node, workflow_service_height_failure(node->height_state));

    if (cache->sensor.imu_cal_generation == s_calibration.status.generation) {
        switch (cache->sensor.imu_cal_state) {
            case NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED:
                workflow_service_copy_string(
                    node->imu_state, sizeof(node->imu_state), "calibrated");
                node->imu_values_valid = true;
                break;
            case NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING:
                workflow_service_copy_string(
                    node->imu_state, sizeof(node->imu_state), "rejected_moving");
                node->imu_values_valid = true;
                break;
            case NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY:
                workflow_service_copy_string(
                    node->imu_state, sizeof(node->imu_state), "rejected_gravity");
                node->imu_values_valid = true;
                break;
            case NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID:
                workflow_service_copy_string(
                    node->imu_state, sizeof(node->imu_state), "sensor_invalid");
                node->imu_values_valid = true;
                break;
            case NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED:
            default:
                break;
        }
        if (node->imu_values_valid) {
            memcpy(node->gyro_bias_mdps, cache->sensor.gyro_bias_mdps,
                sizeof(node->gyro_bias_mdps));
            memcpy(node->gravity_ref_mg, cache->sensor.gravity_ref_mg,
                sizeof(node->gravity_ref_mg));
        }
    }
    workflow_service_add_node_reason(node, workflow_service_imu_failure(node->imu_state));
}

static bool workflow_service_cs_pair_matches(
    const nrf_cs_pair_result_t *pair,
    uint8_t main_node_id,
    uint8_t remote_node_id)
{
    return (pair->node_a == main_node_id && pair->node_b == remote_node_id) ||
        (pair->node_b == main_node_id && pair->node_a == remote_node_id);
}

static void workflow_service_calibration_update_cs(void)
{
    nrf_cs_cycle_snapshot_t cycle;
    if (nrf_topology_service_get_cs_cycle(&cycle) != ESP_OK) return;
    if (s_calibration.previous_cycle_valid &&
        cycle.cycle_id == s_calibration.previous_cycle_id) {
        return;
    }
    if (!s_calibration.cycle_seen) {
        s_calibration.cycle_seen = true;
        s_calibration.cycle_id = cycle.cycle_id;
    } else if (cycle.cycle_id != s_calibration.cycle_id) {
        return;
    }

    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        workflow_calibration_node_t *node = &s_calibration.status.per_node[i + 1U];
        const nrf_cs_pair_result_t *matched = NULL;
        const size_t pair_count = cycle.pair_count < NRF_CS_MAX_PAIRS
            ? cycle.pair_count
            : NRF_CS_MAX_PAIRS;
        for (size_t pair_index = 0U; pair_index < pair_count; ++pair_index) {
            if (workflow_service_cs_pair_matches(
                    &cycle.pairs[pair_index],
                    NRF_TOPOLOGY_MAIN_NODE_ID,
                    s_calibration.node_ids[i])) {
                matched = &cycle.pairs[pair_index];
                break;
            }
        }

        if (matched != NULL && matched->state == NRF_CS_PAIR_VALID) {
            s_calibration.cs_pair_terminal[i] = true;
            node->cs_pairs_ok = matched->spread_mm <=
                WORKFLOW_CALIBRATION_CS_MAX_SPREAD_MM;
            if (!s_calibration.status.max_spread_valid ||
                matched->spread_mm > s_calibration.status.max_spread_mm) {
                s_calibration.status.max_spread_valid = true;
                s_calibration.status.max_spread_mm = matched->spread_mm;
            }
            if (!node->cs_pairs_ok) {
                workflow_service_calibration_add_reason("cs_spread_too_high");
            }
        } else if ((matched != NULL && matched->state != NRF_CS_PAIR_PENDING &&
                    matched->state != NRF_CS_PAIR_UNKNOWN) || cycle.complete) {
            s_calibration.cs_pair_terminal[i] = true;
            node->cs_pairs_ok = false;
            const uint8_t first = NRF_TOPOLOGY_MAIN_NODE_ID < s_calibration.node_ids[i]
                ? NRF_TOPOLOGY_MAIN_NODE_ID
                : s_calibration.node_ids[i];
            const uint8_t second = NRF_TOPOLOGY_MAIN_NODE_ID < s_calibration.node_ids[i]
                ? s_calibration.node_ids[i]
                : NRF_TOPOLOGY_MAIN_NODE_ID;
            char reason[WORKFLOW_CALIBRATION_REASON_MAX];
            (void)snprintf(reason, sizeof(reason), "cs_pair_invalid:%u-%u",
                (unsigned)first, (unsigned)second);
            workflow_service_calibration_add_reason(reason);
        }
    }

    s_calibration.status.pairs_done = 0U;
    bool all_cs_ok = s_calibration.remote_count > 0U;
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        if (s_calibration.cs_pair_terminal[i]) ++s_calibration.status.pairs_done;
        if (!s_calibration.cs_pair_terminal[i] ||
            !s_calibration.status.per_node[i + 1U].cs_pairs_ok) {
            all_cs_ok = false;
        }
    }
    s_calibration.status.per_node[0].cs_pairs_ok = all_cs_ok;
}

static bool workflow_service_calibration_terminal(void)
{
    if (strcmp(s_calibration.status.per_node[0].height_state, "pending") == 0) return false;
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        const workflow_calibration_node_t *node =
            &s_calibration.status.per_node[i + 1U];
        if (strcmp(node->height_state, "pending") == 0 ||
            strcmp(node->imu_state, "pending") == 0 ||
            !s_calibration.cs_pair_terminal[i]) {
            return false;
        }
    }
    return true;
}

static bool workflow_service_calibration_passed(void)
{
    if (strcmp(s_calibration.status.per_node[0].height_state, "valid") != 0) return false;
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        const workflow_calibration_node_t *node =
            &s_calibration.status.per_node[i + 1U];
        if (strcmp(node->height_state, "valid") != 0 ||
            strcmp(node->imu_state, "calibrated") != 0 ||
            !s_calibration.cs_pair_terminal[i] || !node->cs_pairs_ok) {
            return false;
        }
    }
    return true;
}

static void workflow_service_calibration_finish(bool ok)
{
    s_calibration.active = false;
    s_calibration.status.result_valid = true;
    s_calibration.status.result_ok = ok;
    workflow_service_copy_string(
        s_calibration.status.phase,
        sizeof(s_calibration.status.phase),
        ok ? "complete" : "failed");

    const workflow_command_params_t params = {
        .has_ok = true,
        .ok = ok,
        .reason = s_calibration.status.reasons_count > 0U
            ? s_calibration.status.reasons[0]
            : NULL,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    (void)workflow_core_apply(
        &s_core,
        WORKFLOW_COMMAND_CALIBRATION_DONE,
        &params,
        s_assigned_remotes,
        workflow_service_placement_ready());
}

static void workflow_service_calibration_poll(void)
{
    if (!s_calibration.active) return;
    workflow_calibration_node_t old_nodes[WORKFLOW_MAX_CALIBRATION_NODES];
    memcpy(old_nodes, s_calibration.status.per_node, sizeof(old_nodes));

    workflow_service_calibration_update_main_height();
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        nrf_remote_sensor_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        workflow_service_calibration_update_remote(i, &cache);
    }
    workflow_service_calibration_update_cs();

    for (size_t i = 0U; i < s_calibration.status.per_node_count; ++i) {
        const workflow_calibration_node_t *old_node = &old_nodes[i];
        const workflow_calibration_node_t *node = &s_calibration.status.per_node[i];
        if (strcmp(old_node->height_state, node->height_state) != 0 ||
            strcmp(old_node->imu_state, node->imu_state) != 0 ||
            old_node->cs_pairs_ok != node->cs_pairs_ok) {
            workflow_service_calibration_log_node(node);
        }
    }

    const bool terminal = workflow_service_calibration_terminal();
    const uint64_t now_ms = workflow_service_now_ms();
    const bool timed_out = now_ms >= s_calibration.started_ms &&
        now_ms - s_calibration.started_ms >= WORKFLOW_CALIBRATION_TIMEOUT_MS;
    if (terminal) {
        workflow_service_calibration_finish(workflow_service_calibration_passed());
    } else if (timed_out) {
        workflow_service_calibration_add_reason("timeout");
        workflow_service_calibration_finish(false);
    }
}

static void workflow_service_calibration_cancel(void)
{
    if (!s_calibration.active) return;
    s_calibration.active = false;
    workflow_service_copy_string(
        s_calibration.status.phase,
        sizeof(s_calibration.status.phase),
        "cancelled");
}

static void workflow_service_ranging_done(bool success, const char *reason)
{
    if (!success) (void)nrf_topology_service_invalidate_ranging();
    const workflow_command_params_t params = {
        .has_ok = true,
        .ok = success,
        .reason = reason,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    (void)workflow_core_apply(
        &s_core,
        WORKFLOW_COMMAND_RANGING_DONE,
        &params,
        s_assigned_remotes,
        workflow_service_placement_ready());
}

static void workflow_service_ranging_start(void)
{
    (void)nrf_topology_service_invalidate_ranging();
    esp_err_t err = nrf_topology_service_ranging_start(
        &s_workflow_ranging_policy,
        NRF_TOPOLOGY_RANGING_DEFAULT_TIMEOUT_MS);
    if (err == ESP_OK) {
        nrf_topology_ranging_progress_t progress;
        err = nrf_topology_service_ranging_get_progress(&progress);
        if (err == ESP_OK) {
            s_ranging = (workflow_ranging_run_t){
                .active = true,
                .started_at_ms = progress.started_at_ms,
            };
            return;
        }
        (void)nrf_topology_service_invalidate_ranging();
    }

    char reason[WORKFLOW_REASON_MAX];
    (void)snprintf(reason, sizeof(reason), "start_failed:%s", esp_err_to_name(err));
    workflow_service_ranging_done(false, reason);
}

static void workflow_service_step_changed(workflow_step_t previous, workflow_step_t next)
{
    if (previous == next) return;
    if (previous == WORKFLOW_STEP_RANGING && next != WORKFLOW_STEP_RANGING && s_ranging.active) {
        s_ranging.orphaned = true;
    }
    if (next == WORKFLOW_STEP_PLACEMENT) {
        (void)nrf_topology_service_invalidate_ranging();
    }
    if (next == WORKFLOW_STEP_RANGING) workflow_service_ranging_start();
}

static void workflow_service_ranging_poll(void)
{
    if (!s_ranging.active) return;
    nrf_topology_ranging_progress_t progress;
    if (nrf_topology_service_ranging_get_progress(&progress) != ESP_OK ||
        progress.started_at_ms != s_ranging.started_at_ms || !progress.complete) {
        return;
    }

    const bool orphaned = s_ranging.orphaned || s_core.step != WORKFLOW_STEP_RANGING;
    memset(&s_ranging, 0, sizeof(s_ranging));
    if (orphaned) {
        (void)nrf_topology_service_invalidate_ranging();
        return;
    }
    workflow_service_ranging_done(progress.success, progress.failure_reason);
}

static void workflow_service_placement_clear_samples(
    workflow_placement_history_t *history)
{
    history->sample_count = 0U;
    history->last_sample_valid = false;
    history->blocked_until_ms = 0U;
}

static void workflow_service_placement_reset_history(
    workflow_placement_history_t *history)
{
    memset(history, 0, sizeof(*history));
}

static bool workflow_service_placement_update_node(
    workflow_placement_history_t *history,
    uint8_t node_id,
    uint16_t generation,
    bool vertical,
    bool height_valid,
    int64_t height_rel_main_mm,
    uint64_t sample_ms,
    uint64_t now_ms,
    bool collect_sample)
{
    if (!history->node_valid || history->node_id != node_id ||
        history->generation != generation) {
        workflow_service_placement_reset_history(history);
        history->node_valid = true;
        history->node_id = node_id;
        history->generation = generation;
    }
    if (history->vertical_known && history->vertical != vertical) {
        workflow_service_placement_clear_samples(history);
    }
    history->vertical_known = true;
    history->vertical = vertical;

    if (now_ms < sample_ms || !vertical || !height_valid) {
        workflow_service_placement_clear_samples(history);
        return false;
    }
    if (now_ms - sample_ms > WORKFLOW_PLACEMENT_WINDOW_MS) {
        workflow_service_placement_clear_samples(history);
        return false;
    }
    if (now_ms < history->blocked_until_ms) return false;

    while (history->sample_count > 0U &&
           now_ms - history->samples[0].timestamp_ms > WORKFLOW_PLACEMENT_WINDOW_MS) {
        --history->sample_count;
        memmove(
            &history->samples[0],
            &history->samples[1],
            history->sample_count * sizeof(history->samples[0]));
    }

    if (collect_sample &&
        (!history->last_sample_valid || sample_ms > history->last_sample_ms)) {
        if (history->sample_count >= WORKFLOW_PLACEMENT_HISTORY_MAX_SAMPLES) {
            /* ponytail: 32 samples cap RAM; overflow blocks one window rather than lose a live outlier. Raise the cap if polling exceeds 10 Hz. */
            workflow_service_placement_clear_samples(history);
            history->blocked_until_ms = now_ms + WORKFLOW_PLACEMENT_WINDOW_MS;
            return false;
        }
        history->samples[history->sample_count++] = (workflow_placement_sample_t){
            .timestamp_ms = sample_ms,
            .height_rel_main_mm = height_rel_main_mm,
        };
        history->last_sample_ms = sample_ms;
        history->last_sample_valid = true;
    } else if (collect_sample && history->last_sample_valid &&
               sample_ms < history->last_sample_ms) {
        workflow_service_placement_clear_samples(history);
        return false;
    }

    if (history->sample_count < WORKFLOW_PLACEMENT_MIN_SAMPLES) return false;
    int64_t minimum = history->samples[0].height_rel_main_mm;
    int64_t maximum = minimum;
    for (size_t i = 1U; i < history->sample_count; ++i) {
        if (history->samples[i].height_rel_main_mm < minimum) {
            minimum = history->samples[i].height_rel_main_mm;
        }
        if (history->samples[i].height_rel_main_mm > maximum) {
            maximum = history->samples[i].height_rel_main_mm;
        }
    }
    return maximum - minimum <= WORKFLOW_PLACEMENT_MAX_RANGE_MM;
}

static void workflow_service_placement_set_name(
    workflow_placement_node_t *node,
    uint8_t node_id)
{
    if (node_id == NRF_TOPOLOGY_MAIN_NODE_ID) {
        workflow_service_copy_string(node->node, sizeof(node->node), "MAIN");
    } else {
        (void)snprintf(node->node, sizeof(node->node), "G%u", (unsigned)node_id);
    }
}

static void workflow_service_placement_poll(bool collect_samples)
{
    workflow_placement_snapshot_t *status = &s_placement.status;
    memset(status, 0, sizeof(*status));
    const uint64_t now_ms = workflow_service_now_ms();
    const bool in_placement = s_core.step == WORKFLOW_STEP_PLACEMENT;
    device_status_snapshot_t device;
    const bool device_valid = device_status_try_get_snapshot(&device) == ESP_OK;
    const bool main_reference_valid = device_valid &&
        device.main.height_reference.state == DEVICE_MAIN_HEIGHT_REFERENCE_VALID &&
        device.main.height_reference.generation != 0U;
    const bool main_baro_valid = device_valid && device.main.bmp280_valid &&
        device.main.pressure_pa > 0U;
    const bool main_height_valid = main_reference_valid && main_baro_valid;
    const uint16_t main_generation = main_reference_valid
        ? device.main.height_reference.generation
        : 0U;
    const bool take_samples = in_placement && collect_samples;

    if (!in_placement) {
        for (size_t i = 0U; i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
            workflow_service_placement_reset_history(&s_placement.history[i]);
        }
    }

    workflow_placement_node_t *main_node = &status->per_node[status->per_node_count++];
    workflow_service_placement_set_name(main_node, NRF_TOPOLOGY_MAIN_NODE_ID);
    main_node->vertical = true;
    main_node->height_rel_main_mm_valid = main_height_valid;
    main_node->height_rel_main_mm = 0;
    main_node->altitude_asl_m_valid = main_baro_valid;
    if (main_baro_valid) {
        main_node->altitude_asl_m =
            (double)nrf_baro_pressure_to_altitude_mm(device.main.pressure_pa) / 1000.0;
    }
    main_node->stable = workflow_service_placement_update_node(
        &s_placement.history[0],
        NRF_TOPOLOGY_MAIN_NODE_ID,
        main_generation,
        main_node->vertical,
        main_height_valid,
        0,
        now_ms,
        now_ms,
        take_samples);

    nrf_topology_snapshot_t topology;
    const bool topology_valid = nrf_topology_service_get_snapshot(&topology) == ESP_OK;
    bool assigned[NRF_TOPOLOGY_REMOTE_COUNT] = { false };
    bool all_stable = topology_valid && in_placement && main_node->stable;
    if (topology_valid) {
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            const nrf_topology_node_snapshot_t *remote = &topology.remotes[channel];
            if (remote->state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
            assigned[channel] = true;
            workflow_placement_node_t *node =
                &status->per_node[status->per_node_count++];
            workflow_service_placement_set_name(node, remote->node_id);

            nrf_remote_sensor_cache_t cache;
            memset(&cache, 0, sizeof(cache));
            const bool cache_read =
                nrf_topology_service_get_sensor_snapshot(channel, &cache) == ESP_OK;
            const bool cache_current = cache_read && cache.identity_valid &&
                cache.topology_current && cache.response_valid &&
                cache.identity.node_id == remote->node_id &&
                now_ms >= cache.esp_receipt_uptime_ms &&
                now_ms - cache.esp_receipt_uptime_ms <= WORKFLOW_PLACEMENT_WINDOW_MS;
            const bool environment_valid = cache_current &&
                (cache.sensor.valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U &&
                cache.sensor.pressure_pa > 0U;
            node->vertical = cache_current &&
                cache.sensor.orientation_state == NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
            node->height_rel_main_mm_valid = main_height_valid && environment_valid &&
                cache.sensor.reference_state == NRF_UPSTREAM_REFERENCE_VALID &&
                cache.sensor.reference_generation == main_generation;
            if (node->height_rel_main_mm_valid) {
                node->height_rel_main_mm =
                    (int64_t)cache.sensor.relative_z_mm -
                    (int64_t)device.main.height_reference.relative_z_mm;
            }
            node->altitude_asl_m_valid = environment_valid;
            if (environment_valid) {
                node->altitude_asl_m = (double)cache.sensor.altitude_mm / 1000.0;
            }
            node->stable = workflow_service_placement_update_node(
                &s_placement.history[(size_t)channel + 1U],
                remote->node_id,
                node->height_rel_main_mm_valid ? main_generation : 0U,
                node->vertical,
                node->height_rel_main_mm_valid,
                node->height_rel_main_mm,
                cache.esp_receipt_uptime_ms,
                now_ms,
                take_samples && cache_current);
            all_stable = all_stable && node->stable;
        }
    }
    for (size_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (!assigned[channel]) {
            workflow_service_placement_reset_history(&s_placement.history[channel + 1U]);
        }
    }
    status->all_stable = all_stable;
}

static uint8_t workflow_service_assigned_remote_count(void)
{
    nrf_topology_snapshot_t topology;
    if (nrf_topology_service_get_snapshot(&topology) != ESP_OK) return 0U;

    size_t count = 0U;
    for (size_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        if (topology.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED) ++count;
    }
    return count > UINT8_MAX ? UINT8_MAX : (uint8_t)count;
}

static esp_err_t workflow_service_active_object(
    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE],
    bool *active_out
)
{
    if (object_id == NULL || active_out == NULL) return ESP_ERR_INVALID_ARG;
    object_id[0] = '\0';
    *active_out = false;
    object_service_summary_t object;
    const esp_err_t err = object_service_get_active(&object);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (!object.active || object.id[0] == '\0') return ESP_OK;
    (void)snprintf(object_id, OBJECT_SERVICE_OBJECT_ID_SIZE, "%s", object.id);
    *active_out = true;
    return ESP_OK;
}

static void workflow_service_reconcile_locked(void)
{
    const workflow_step_t previous_step = s_core.step;
    measurement_service_snapshot_t measurement;
    const esp_err_t measurement_err = measurement_service_get_snapshot(&measurement);

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = {0};
    bool active_object_present = false;
    if (workflow_service_active_object(object_id, &active_object_present) == ESP_OK &&
        strcmp(object_id, s_active_object_id) != 0) {
        workflow_service_calibration_cancel();
        (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
        workflow_core_reset_object(&s_core, active_object_present, true);
    }

    s_assigned_remotes = workflow_service_assigned_remote_count();
    if (measurement_err != ESP_OK) {
        workflow_service_step_changed(previous_step, s_core.step);
        return;
    }

    if (measurement.state == MEASUREMENT_SERVICE_LIVE) {
        s_recorder_reconciliation_armed = true;
        workflow_service_step_changed(previous_step, s_core.step);
        return;
    }
    if (!s_recorder_reconciliation_armed) {
        workflow_service_step_changed(previous_step, s_core.step);
        return;
    }

    if (measurement.state == MEASUREMENT_SERVICE_STARTING ||
        measurement.state == MEASUREMENT_SERVICE_RECORDING) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_RECORDING, NULL);
    }
    else if (measurement.state == MEASUREMENT_SERVICE_REVIEW &&
        s_core.step == WORKFLOW_STEP_RECORDING) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_STOPPED, NULL);
    }
    else if (measurement.state == MEASUREMENT_SERVICE_ERROR &&
        (s_core.step == WORKFLOW_STEP_RECORDING || s_core.step == WORKFLOW_STEP_STOPPED)) {
        workflow_core_reconcile(&s_core, WORKFLOW_STEP_STOPPED, "recording_error");
    }
    workflow_service_step_changed(previous_step, s_core.step);
}

esp_err_t workflow_service_init(void)
{
    if (s_initialized) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = {0};
    bool active_object_present = false;
    (void)workflow_service_active_object(object_id, &active_object_present);
    (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
    workflow_core_init(&s_core, active_object_present);
    s_assigned_remotes = workflow_service_assigned_remote_count();
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;
    memset(&s_calibration, 0, sizeof(s_calibration));
    memset(&s_placement, 0, sizeof(s_placement));
    memset(&s_ranging, 0, sizeof(s_ranging));
    workflow_service_copy_string(
        s_calibration.status.phase,
        sizeof(s_calibration.status.phase),
        "idle");

    measurement_service_snapshot_t measurement;
    s_recorder_reconciliation_armed =
        measurement_service_get_snapshot(&measurement) == ESP_OK &&
        measurement.state == MEASUREMENT_SERVICE_LIVE;
    s_initialized = true;
    return ESP_OK;
}

void workflow_service_shutdown(void)
{
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_initialized = false;
    s_recorder_reconciliation_armed = false;
    s_assigned_remotes = 0U;
    s_active_object_id[0] = '\0';
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;
    memset(&s_calibration, 0, sizeof(s_calibration));
    memset(&s_placement, 0, sizeof(s_placement));
    memset(&s_ranging, 0, sizeof(s_ranging));
    memset(&s_core, 0, sizeof(s_core));
}

esp_err_t workflow_service_poll(void)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    workflow_service_reconcile_locked();
    workflow_service_calibration_poll();
    workflow_service_ranging_poll();
    workflow_service_placement_poll(true);
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    const esp_err_t poll_err = workflow_service_poll();
    if (poll_err != ESP_OK) return poll_err;
    if (!workflow_service_lock()) return ESP_ERR_INVALID_STATE;

    workflow_core_snapshot(
        &s_core,
        s_assigned_remotes,
        workflow_service_placement_ready(),
        snapshot
    );
    snapshot->calibration = s_calibration.status;
    snapshot->placement = s_placement.status;
    workflow_service_unlock();
    return ESP_OK;
}

workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
)
{
    if (params == NULL) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    if (!s_initialized || !workflow_service_lock()) return WORKFLOW_COMMAND_RESULT_UNAVAILABLE;
    workflow_service_reconcile_locked();
    workflow_service_placement_poll(false);

    const workflow_core_t previous = s_core;
    const workflow_command_result_t result = workflow_core_apply(
        &s_core,
        command,
        params,
        s_assigned_remotes,
        workflow_service_placement_ready()
    );
    if (result != WORKFLOW_COMMAND_RESULT_OK) {
        workflow_service_unlock();
        return result;
    }

    if (s_core.step != previous.step) {
        workflow_service_placement_poll(false);
        workflow_service_step_changed(previous.step, s_core.step);
    }

    if (s_core.step == WORKFLOW_STEP_CALIBRATING &&
        previous.step != WORKFLOW_STEP_CALIBRATING) {
        if (workflow_service_calibration_start() != ESP_OK) {
            s_core = previous;
            workflow_service_unlock();
            return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
        }
    } else if (previous.step == WORKFLOW_STEP_CALIBRATING &&
               s_core.step != WORKFLOW_STEP_CALIBRATING) {
        workflow_service_calibration_cancel();
    }

    esp_err_t measurement_err = ESP_OK;
    if (command == WORKFLOW_COMMAND_START_RECORDING) {
        measurement_err = measurement_service_start_async();
        if (measurement_err == ESP_OK) s_recorder_reconciliation_armed = true;
    }
    else if (command == WORKFLOW_COMMAND_STOP_RECORDING) {
        measurement_err = measurement_service_stop_async();
    }
    else if (command == WORKFLOW_COMMAND_DELETE_RECORDING) {
        measurement_err = measurement_service_delete_async();
    }
    if (measurement_err != ESP_OK) {
        s_core = previous;
        workflow_service_unlock();
        return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    }

    workflow_service_unlock();
    return WORKFLOW_COMMAND_RESULT_OK;
}

esp_err_t workflow_service_set_placement_ready_predicate(
    workflow_service_placement_ready_fn predicate,
    void *context
)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    s_placement_ready_predicate = predicate;
    s_placement_ready_context = context;
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_set_progress(int progress)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    const bool updated = workflow_core_set_progress(&s_core, progress);
    workflow_service_unlock();
    return updated ? ESP_OK : ESP_ERR_INVALID_ARG;
}
