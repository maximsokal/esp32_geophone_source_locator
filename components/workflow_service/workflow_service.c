#include "workflow_service.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>

#include "app_tick.h"
#include "app_task_config.h"
#include "app_stop_timeout_config.h"
#include "device_status.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "localization_config.h"
#include "localization_job.h"
#include "localization_material_presets.h"
#include "main_barometer.h"
#include "measurement_service.h"
#include "nrf_baro_altitude.h"
#include "nrf_topology_service.h"
#include "object_service.h"
#include "object_store.h"
#include "cJSON.h"

#define WORKFLOW_CALIBRATION_TIMEOUT_MS 15000U
/*
 * A REMOTE that cannot take the IMU calibration command (its sensor session is busy, e.g. still finishing
 * the height-reference refresh) drops it and says so only in its own TWIS log; the I2C write is ACKed.
 * 2026-10-31: G1 stayed imu=pending until the timeout. The command is repeated at this interval for the
 * nodes that have not answered; a node that already runs a capture refuses the repeat as busy, harmlessly.
 */
#define WORKFLOW_CALIBRATION_IMU_RETRY_MS 3000U
/* How long a new RANGING start waits for a cancelled run to wind down before it is reported as failed. */
#define WORKFLOW_RANGING_RESTART_WAIT_MS 10000U
/* WORKFLOW_PLACEMENT_WINDOW_MS at the poll period, plus headroom (35 samples at 200 ms). */
#define WORKFLOW_PLACEMENT_HISTORY_MAX_SAMPLES 96U
/* While on a placement step each node's stability state is logged this often, and at once on a change. */
#define WORKFLOW_PLACEMENT_LOG_PERIOD_MS 2000U
/*
 * device_status_try_get_snapshot() takes its lock without waiting, so a poll that lands while a writer holds
 * it gets nothing. MAIN's last good status is reused for this long rather than wiping every node's history.
 */
#define WORKFLOW_PLACEMENT_MAIN_HOLD_MS 1000U
#define WORKFLOW_SERVICE_POLL_STACK_BYTES 8192U
#define WORKFLOW_CALC_SETTINGS_MAX_JSON_DEPTH 8U
#define WORKFLOW_SERVICE_STATUS_REFRESH_MS 2000U
#define WORKFLOW_SERVICE_POLL_TASK_PRIORITY 1U
#define WORKFLOW_ASYNC_QUEUE_DEPTH 4U
#define WORKFLOW_CALC_MATERIAL_ID_SIZE 24U

/*
 * 8 KiB is the fixed Xtensa budget for the deepest branch: poll task (48 B) ->
 * poll (32 B) -> async dispatch (80 B) -> command (144 B) -> calculation start
 * (112 B), then an SD settings read, cJSON parse (depth capped at 8), config
 * parse, or job submit. Large poll snapshots and object data are in PSRAM
 * scratch. workflow_snapshot_t is a caller-owned output, never a poll-task local.
 */
static const char *TAG = "WORKFLOW";

typedef struct {
    workflow_calibration_snapshot_t status;
    bool active;
    uint8_t remote_count;
    uint8_t channels[WORKFLOW_MAX_REMOTE_NODES];
    uint8_t node_ids[WORKFLOW_MAX_REMOTE_NODES];
    uint64_t started_ms;
    uint64_t imu_request_ms;
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
    bool reason_logged;
    workflow_placement_reason_t logged_reason;
    size_t sample_count;
    workflow_placement_sample_t samples[WORKFLOW_PLACEMENT_HISTORY_MAX_SAMPLES];
} workflow_placement_history_t;

typedef struct {
    workflow_placement_snapshot_t status;
    workflow_placement_history_t history[WORKFLOW_MAX_CALIBRATION_NODES];
    bool main_cached;
    uint64_t main_cached_ms;
    device_main_status_t main_cache;
    uint64_t next_log_ms;
} workflow_placement_run_t;

typedef struct {
    bool active;
    bool orphaned;
    bool restart_pending;
    uint64_t started_at_ms;
    uint64_t restart_deadline_ms;
} workflow_ranging_run_t;

typedef enum {
    WORKFLOW_ASYNC_COMMAND = 0,
    WORKFLOW_ASYNC_MATERIAL_SAVE,
} workflow_async_kind_t;

typedef struct {
    uint32_t seq;
    workflow_async_kind_t kind;
    workflow_command_t command;
    bool has_generation;
    uint32_t expected_generation;
    workflow_command_params_t distance;
    char material_id[WORKFLOW_CALC_MATERIAL_ID_SIZE];
    uint32_t custom_v_mps;
    uint32_t custom_sigma_mps;
    bool has_range;
    uint32_t range_min_mps;
    uint32_t range_max_mps;
} workflow_async_command_t;

typedef struct {
    device_status_snapshot_t device;
    localization_job_status_t job_status;
    localization_calculate_config_t calculation_config;
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t remote_sensor;
    nrf_topology_ranging_progress_t ranging_progress;
    object_store_profile_t ranging_object;
    object_store_cs_pairs_t ranging_pairs;
    workflow_calibration_node_t old_calibration_nodes[WORKFLOW_MAX_CALIBRATION_NODES];
} workflow_poll_scratch_t;

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_async_enqueue_mutex;
static QueueHandle_t s_async_queue;
static TaskHandle_t s_poll_task;
static SemaphoreHandle_t s_poll_stop_ack;
static atomic_bool s_poll_stop_requested;
static atomic_uint_least32_t s_poll_missed_deadlines;
static bool s_poll_join_pending;
static atomic_bool s_poll_hwm_after_first_calculation;
static bool s_poll_first_calculation_seen;
static bool s_initialized;
static bool s_recorder_reconciliation_armed;
static uint8_t s_assigned_remotes;
static char s_active_object_id[OBJECT_STORE_ID_SIZE];
static char s_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
static uint32_t s_async_next_seq;
static size_t s_async_queue_occupancy;
static workflow_service_queue_stats_t s_async_queue_stats;
static workflow_last_async_t s_last_async;
static workflow_last_material_save_t s_last_material_save;
static bool s_placement_ready;
static bool s_has_scan;
static bool s_scan_storage_available;
static bool s_scan_cache_valid;
static uint64_t s_scan_cache_updated_ms;
static workflow_step_t s_scan_cache_step;
static char s_scan_cache_object_id[OBJECT_STORE_ID_SIZE];
static char s_scan_cache_session_id[SEISMIC_RECORDER_SESSION_ID_SIZE];
static bool s_calc_settings_ready;
static bool s_calc_settings_cache_valid;
static uint64_t s_calc_settings_cache_updated_ms;
static char s_calc_settings_cache_object_id[OBJECT_STORE_ID_SIZE];
static char s_calc_material[WORKFLOW_CALC_MATERIAL_ID_SIZE];
static bool s_calc_velocity_range_set;
static float s_calc_velocity_range_min_mps;
static float s_calc_velocity_range_max_mps;
static uint32_t s_calculation_job_id;
static uint32_t s_orphaned_job_id;
static workflow_core_t s_core;
static workflow_service_placement_ready_fn s_placement_ready_predicate;
static void *s_placement_ready_context;
static EXT_RAM_BSS_ATTR workflow_calibration_run_t s_calibration;
static EXT_RAM_BSS_ATTR workflow_placement_run_t s_placement;
static EXT_RAM_BSS_ATTR workflow_ranging_run_t s_ranging;
static EXT_RAM_BSS_ATTR workflow_poll_scratch_t s_poll_scratch;

static workflow_command_result_t workflow_service_command_locked(
    workflow_command_t command,
    const workflow_command_params_t *params
);
static void workflow_service_process_async_locked(void);
static void workflow_service_refresh_calc_settings_locked(uint64_t now_ms);
static void workflow_service_mark_calculation_orphaned_locked(void);
static BaseType_t workflow_service_create_poll_task(TaskHandle_t *task_out);
static void workflow_service_poll_task(void *argument);


static void workflow_service_record_async_enqueue(bool accepted)
{
    if (accepted) {
        ++s_async_queue_occupancy;
        if (s_async_queue_stats.peak_occupancy < s_async_queue_occupancy) {
            s_async_queue_stats.peak_occupancy = (uint32_t)s_async_queue_occupancy;
        }
    } else if (s_async_queue_stats.overflow_count < UINT32_MAX) {
        ++s_async_queue_stats.overflow_count;
    }
}


static BaseType_t workflow_service_receive_async(workflow_async_command_t *item)
{
    if (
        item == NULL || s_async_enqueue_mutex == NULL ||
        xSemaphoreTake(s_async_enqueue_mutex, 0U) != pdTRUE
    ) {
        return pdFALSE;
    }
    const BaseType_t received = xQueueReceive(s_async_queue, item, 0U);
    if (received == pdTRUE && s_async_queue_occupancy > 0U) {
        --s_async_queue_occupancy;
    }
    xSemaphoreGive(s_async_enqueue_mutex);
    return received;
}

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

static bool workflow_service_calc_settings_depth_ok(const char *json)
{
    size_t depth = 0U;
    bool in_string = false;
    bool escaped = false;
    if (json == NULL) return false;

    for (const char *cursor = json; *cursor != '\0'; ++cursor) {
        const char value = *cursor;
        if (in_string) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') in_string = false;
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            if (++depth > WORKFLOW_CALC_SETTINGS_MAX_JSON_DEPTH) return false;
        } else if (value == '}' || value == ']') {
            if (depth == 0U) return false;
            --depth;
        }
    }
    return !in_string && depth == 0U;
}

/* Called only while s_mutex is held; settings are a flat scalar schema. */
static esp_err_t workflow_service_parse_calc_settings_locked(
    const char *json,
    uint32_t *budget_ms_out,
    localization_calculate_config_t *config_out,
    char material_out[WORKFLOW_CALC_MATERIAL_ID_SIZE],
    bool *velocity_range_present_out
)
{
    if (velocity_range_present_out != NULL) *velocity_range_present_out = false;
    if (json == NULL || budget_ms_out == NULL) return ESP_ERR_INVALID_ARG;
    if (!workflow_service_calc_settings_depth_ok(json)) return ESP_ERR_INVALID_RESPONSE;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    localization_calculate_config_t *config = config_out != NULL
        ? config_out : &s_poll_scratch.calculation_config;
    const esp_err_t err = localization_config_from_json(
        root, config, budget_ms_out);
    if (err == ESP_OK && material_out != NULL) {
        workflow_service_copy_string(material_out, WORKFLOW_CALC_MATERIAL_ID_SIZE,
            config->material[0] != '\0' ? config->material : "custom");
    }
    if (err == ESP_OK && velocity_range_present_out != NULL) {
        *velocity_range_present_out =
            cJSON_GetObjectItemCaseSensitive(root, "velocity_range") != NULL;
    }
    cJSON_Delete(root);
    return err;
}

static uint64_t workflow_service_now_ms(void)
{
    const int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)now_us / UINT64_C(1000) : 0U;
}

static bool workflow_service_step_has_session(workflow_step_t step)
{
    return step == WORKFLOW_STEP_RECORDING || step == WORKFLOW_STEP_STOPPED ||
        step == WORKFLOW_STEP_CALCULATING || step == WORKFLOW_STEP_RESULT;
}

static void workflow_service_update_session_locked(
    esp_err_t measurement_err,
    const measurement_service_snapshot_t *measurement
)
{
    if (!workflow_service_step_has_session(s_core.step)) {
        if (s_session_id[0] != '\0') {
            s_session_id[0] = '\0';
            s_scan_cache_valid = false;
            s_has_scan = false;
        }
        return;
    }
    if (measurement_err != ESP_OK || measurement == NULL ||
        strcmp(measurement->object_id, s_active_object_id) != 0 ||
        measurement->session_id[0] == '\0') {
        return;
    }
    const bool recording = s_core.step == WORKFLOW_STEP_RECORDING &&
        (measurement->state == MEASUREMENT_SERVICE_STARTING ||
         measurement->state == MEASUREMENT_SERVICE_RECORDING);
    const bool reviewing = s_core.step != WORKFLOW_STEP_RECORDING &&
        measurement->state == MEASUREMENT_SERVICE_REVIEW;
    if (recording || reviewing) {
        if (strcmp(s_session_id, measurement->session_id) != 0) {
            s_scan_cache_valid = false;
            s_has_scan = false;
        }
        workflow_service_copy_string(s_session_id, sizeof(s_session_id), measurement->session_id);
    }
}

static void workflow_service_refresh_calc_settings_locked(uint64_t now_ms)
{
    const bool object_changed = strcmp(s_calc_settings_cache_object_id, s_active_object_id) != 0;
    if (!s_calc_settings_cache_valid || object_changed ||
        now_ms - s_calc_settings_cache_updated_ms >= WORKFLOW_SERVICE_STATUS_REFRESH_MS) {
        s_calc_settings_ready = false;
        s_calc_material[0] = '\0';
        s_calc_velocity_range_set = false;
        s_calc_velocity_range_min_mps = 0.0f;
        s_calc_velocity_range_max_mps = 0.0f;
        if (s_active_object_id[0] != '\0') {
            char *json = NULL;
            if (object_store_get_calc_settings_json_cached(s_active_object_id, &json) == ESP_OK && json != NULL) {
                uint32_t budget_ms = 0U;
                bool range_present = false;
                s_calc_settings_ready = workflow_service_parse_calc_settings_locked(
                    json, &budget_ms, &s_poll_scratch.calculation_config, s_calc_material,
                    &range_present) == ESP_OK;
                if (s_calc_settings_ready && range_present &&
                    s_poll_scratch.calculation_config.velocity_range.bounds.set) {
                    s_calc_velocity_range_set = true;
                    s_calc_velocity_range_min_mps =
                        s_poll_scratch.calculation_config.velocity_range.bounds.v_min_mps;
                    s_calc_velocity_range_max_mps =
                        s_poll_scratch.calculation_config.velocity_range.bounds.v_max_mps;
                }
                free(json);
            }
        }
        workflow_core_set_calc_settings_ready(&s_core, s_calc_settings_ready);
        workflow_service_copy_string(
            s_calc_settings_cache_object_id, sizeof(s_calc_settings_cache_object_id), s_active_object_id);
        s_calc_settings_cache_updated_ms = now_ms;
        s_calc_settings_cache_valid = true;
    }
}

static void workflow_service_refresh_presence_locked(void)
{
    const uint64_t now_ms = workflow_service_now_ms();
    const bool identity_changed =
        s_scan_cache_step != s_core.step ||
        strcmp(s_scan_cache_object_id, s_active_object_id) != 0 ||
        strcmp(s_scan_cache_session_id, s_session_id) != 0;
    if (!s_scan_cache_valid || identity_changed ||
        now_ms - s_scan_cache_updated_ms >= WORKFLOW_SERVICE_STATUS_REFRESH_MS) {
        s_has_scan = false;
        s_scan_storage_available = false;
        if (s_active_object_id[0] != '\0') {
            bool present = false;
            if (object_store_scan_present(s_active_object_id, s_session_id, &present) == ESP_OK) {
                s_scan_storage_available = true;
                s_has_scan = present;
                if (!s_has_scan && s_session_id[0] != '\0') {
                    bool staged = false;
                    if (object_store_scan_present(s_active_object_id, "", &staged) == ESP_OK) {
                        s_has_scan = staged;
                    }
                }
            }
        }
        workflow_service_copy_string(s_scan_cache_object_id, sizeof(s_scan_cache_object_id), s_active_object_id);
        workflow_service_copy_string(s_scan_cache_session_id, sizeof(s_scan_cache_session_id), s_session_id);
        s_scan_cache_step = s_core.step;
        s_scan_cache_updated_ms = now_ms;
        s_scan_cache_valid = true;
    }

    workflow_service_refresh_calc_settings_locked(now_ms);
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
        "WORKFLOW_CAL node=%s height=%s imu=%s",
        node->node,
        node->height_state,
        node->imu_state);
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

static void workflow_service_calibration_finish(bool ok);

static esp_err_t workflow_service_calibration_start(void)
{
    if (s_assigned_remotes == 0U) {
        memset(&s_calibration, 0, sizeof(s_calibration));
        workflow_service_copy_string(
            s_calibration.status.phase,
            sizeof(s_calibration.status.phase),
            "running");
        s_calibration.status.per_node_count = 1U;
        workflow_service_calibration_start_name(
            &s_calibration.status.per_node[0], NRF_TOPOLOGY_MAIN_NODE_ID);
        workflow_service_calibration_add_reason("insufficient_nodes");
        workflow_service_calibration_finish(false);
        return ESP_OK;
    }

    nrf_topology_snapshot_t *topology = &s_poll_scratch.topology;
    esp_err_t err = nrf_topology_service_get_snapshot(topology);
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
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED ||
            topology->remotes[channel].node_id == NRF_TOPOLOGY_MAIN_NODE_ID ||
            remote_count >= WORKFLOW_MAX_REMOTE_NODES) {
            return ESP_ERR_INVALID_STATE;
        }
        channels[remote_count] = channel;
        node_ids[remote_count] = topology->remotes[channel].node_id;
        ++remote_count;
    }
    if (remote_count == 0U) return ESP_ERR_NOT_FOUND;

    err = nrf_topology_service_request_imu_calibration(generation, channel_mask);
    if (err != ESP_OK) return err;

    memset(&s_calibration, 0, sizeof(s_calibration));
    s_calibration.active = true;
    s_calibration.remote_count = (uint8_t)remote_count;
    memcpy(s_calibration.channels, channels, remote_count * sizeof(channels[0]));
    memcpy(s_calibration.node_ids, node_ids, remote_count * sizeof(node_ids[0]));
    s_calibration.started_ms = workflow_service_now_ms();
    s_calibration.imu_request_ms = s_calibration.started_ms;
    s_calibration.status.generation = generation;
    workflow_service_copy_string(
        s_calibration.status.phase,
        sizeof(s_calibration.status.phase),
        "running");
    s_calibration.status.per_node_count = remote_count + 1U;
    /* Calibration is height + IMU only: Bluetooth stays idle until ranging, after placement. */
    s_calibration.status.pairs_total = 0U;
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
    device_status_snapshot_t *device = &s_poll_scratch.device;
    if (device_status_try_get_snapshot(device) != ESP_OK) return;
    workflow_calibration_node_t *node = &s_calibration.status.per_node[0];
    if (device->main.height_reference.generation != s_calibration.status.generation) return;

    switch (device->main.height_reference.state) {
        case DEVICE_MAIN_HEIGHT_REFERENCE_VALID:
            workflow_service_copy_string(node->height_state, sizeof(node->height_state), "valid");
            node->relative_z_mm_valid = true;
            node->relative_z_mm = device->main.height_reference.relative_z_mm;
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

static bool workflow_service_calibration_terminal(void)
{
    if (strcmp(s_calibration.status.per_node[0].height_state, "pending") == 0) return false;
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        const workflow_calibration_node_t *node =
            &s_calibration.status.per_node[i + 1U];
        if (strcmp(node->height_state, "pending") == 0 ||
            strcmp(node->imu_state, "pending") == 0) {
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
            strcmp(node->imu_state, "calibrated") != 0) {
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
    workflow_calibration_node_t *old_nodes = s_poll_scratch.old_calibration_nodes;
    memcpy(old_nodes, s_calibration.status.per_node, sizeof(s_poll_scratch.old_calibration_nodes));

    workflow_service_calibration_update_main_height();
    for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
        nrf_remote_sensor_cache_t *cache = &s_poll_scratch.remote_sensor;
        memset(cache, 0, sizeof(*cache));
        workflow_service_calibration_update_remote(i, cache);
    }

    for (size_t i = 0U; i < s_calibration.status.per_node_count; ++i) {
        const workflow_calibration_node_t *old_node = &old_nodes[i];
        const workflow_calibration_node_t *node = &s_calibration.status.per_node[i];
        if (strcmp(old_node->height_state, node->height_state) != 0 ||
            strcmp(old_node->imu_state, node->imu_state) != 0) {
            workflow_service_calibration_log_node(node);
        }
    }

    const bool terminal = workflow_service_calibration_terminal();
    const uint64_t now_ms = workflow_service_now_ms();
    const bool timed_out = now_ms >= s_calibration.started_ms &&
        now_ms - s_calibration.started_ms >= WORKFLOW_CALIBRATION_TIMEOUT_MS;
    if (!terminal && !timed_out && now_ms >= s_calibration.imu_request_ms &&
        now_ms - s_calibration.imu_request_ms >= WORKFLOW_CALIBRATION_IMU_RETRY_MS) {
        uint8_t silent_mask = 0U;
        for (size_t i = 0U; i < s_calibration.remote_count; ++i) {
            if (strcmp(s_calibration.status.per_node[i + 1U].imu_state, "pending") == 0) {
                silent_mask |= (uint8_t)(1U << s_calibration.channels[i]);
            }
        }
        if (silent_mask != 0U) {
            const esp_err_t repeat_err = nrf_topology_service_request_imu_calibration(
                s_calibration.status.generation, silent_mask);
            ESP_LOGW(
                TAG,
                "WORKFLOW_CAL imu command repeated generation=%u channels=0x%02x result=%s",
                (unsigned)s_calibration.status.generation,
                (unsigned)silent_mask,
                esp_err_to_name(repeat_err));
        }
        s_calibration.imu_request_ms = now_ms;
    }
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

static void workflow_service_ranging_done(bool success, bool partial, const char *reason)
{
    (void)partial;
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

static esp_err_t workflow_service_ranging_load_limits(uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT])
{
    memset(limits, 0, NRF_TOPOLOGY_REMOTE_COUNT * sizeof(limits[0]));
    object_store_profile_t *profile = &s_poll_scratch.ranging_object;
    esp_err_t err = object_store_get_profile_cached(s_active_object_id, profile);
    if (err == ESP_OK) {
        for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
            const float length = profile->cable_lengths_m[i];
            if (!isfinite(length) || length <= 0.0f || length > (float)INT32_MAX / 1000.0f) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            limits[i] = (uint32_t)(length * 1000.0f);
        }
    }
    return err;
}

static void workflow_service_ranging_start(void)
{
    if (s_assigned_remotes < 3U) {
        workflow_service_ranging_done(false, false, "insufficient_nodes");
        return;
    }
    (void)nrf_topology_service_invalidate_ranging();
    uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT];
    esp_err_t err = workflow_service_ranging_load_limits(limits);
    if (err == ESP_OK) {
        err = nrf_topology_service_ranging_start_autonomous_limited(&s_workflow_ranging_policy, limits);
        if (err == ESP_ERR_INVALID_STATE && s_ranging.active && s_ranging.orphaned) {
            /* The run cancelled by BACK is still winding down; poll starts this one once it has stopped. */
            s_ranging.restart_pending = true;
            s_ranging.restart_deadline_ms = workflow_service_now_ms() + WORKFLOW_RANGING_RESTART_WAIT_MS;
            return;
        }
    }
    if (err == ESP_OK) {
        nrf_topology_ranging_progress_t *progress = &s_poll_scratch.ranging_progress;
        err = nrf_topology_service_ranging_get_progress(progress);
        if (err == ESP_OK) {
            s_ranging = (workflow_ranging_run_t){
                .active = true,
                .started_at_ms = progress->started_at_ms,
            };
            return;
        }
        (void)nrf_topology_service_invalidate_ranging();
    }

    char reason[WORKFLOW_REASON_MAX];
    (void)snprintf(reason, sizeof(reason), "start_failed:%s", esp_err_to_name(err));
    (void)nrf_topology_service_invalidate_ranging();
    workflow_service_ranging_done(false, false, reason);
}

static void workflow_service_step_changed(workflow_step_t previous, workflow_step_t next)
{
    if (previous == next) return;
    if (next == WORKFLOW_STEP_SENSOR_CHECK) main_barometer_request_polling();
    if (previous == WORKFLOW_STEP_RANGING && next != WORKFLOW_STEP_RANGING && s_ranging.active) {
        s_ranging.orphaned = true;
        (void)nrf_topology_service_ranging_cancel();
        (void)nrf_topology_service_invalidate_ranging();
    }
    else if (next == WORKFLOW_STEP_PLACEMENT) {
        (void)nrf_topology_service_invalidate_ranging();
    }
    if (next == WORKFLOW_STEP_RANGING) workflow_service_ranging_start();
}

static esp_err_t workflow_service_ranging_save(const nrf_topology_ranging_progress_t *progress)
{
    if (progress->pair_count > 0U) {
        object_store_cs_pairs_t *pairs = &s_poll_scratch.ranging_pairs;
        memset(pairs, 0, sizeof(*pairs));
        if (progress->pair_count > OBJECT_STORE_MAX_CS_PAIRS) {
            return ESP_ERR_INVALID_SIZE;
        }
        pairs->pair_count = progress->pair_count;
        for (size_t i = 0U; i < pairs->pair_count; ++i) {
            const nrf_cs_pair_result_t *source = &progress->pairs[i];
            object_store_cs_pair_t *dest = &pairs->pairs[i];
            dest->node_a = source->node_a; dest->node_b = source->node_b;
            dest->valid = source->state == NRF_CS_PAIR_VALID;
            const char *pair_state = source->state == NRF_CS_PAIR_VALID ? "valid"
                : source->state == NRF_CS_PAIR_CABLE_EXCEEDED ? "cable_exceeded"
                : source->state == NRF_CS_PAIR_RETRY_EXHAUSTED ? "retry_exhausted"
                : source->state == NRF_CS_PAIR_PENDING ? "pending" : "missing";
            (void)snprintf(dest->state, sizeof(dest->state), "%s", pair_state);
            dest->distance_mm = source->distance_mm; dest->confidence = source->confidence;
            dest->attempts = source->attempts; dest->valid_samples = source->valid_samples;
            dest->spread_mm = source->spread_mm; dest->rejected_samples = source->rejected_samples;
            dest->scheduler_quality_flags = source->scheduler_quality_flags;
            dest->automatic_distance_mm = source->automatic_distance_mm;
        }
        return object_store_set_ranging_pairs(s_active_object_id, pairs);
    }
    return ESP_OK;
}

static void workflow_service_ranging_poll(void)
{
    if (!s_ranging.active) return;
    nrf_topology_ranging_progress_t *progress = &s_poll_scratch.ranging_progress;
    const bool finished = nrf_topology_service_ranging_get_progress(progress) == ESP_OK &&
        progress->started_at_ms == s_ranging.started_at_ms && progress->complete;
    if (!finished) {
        if (s_ranging.restart_pending && s_core.step == WORKFLOW_STEP_RANGING &&
            workflow_service_now_ms() >= s_ranging.restart_deadline_ms) {
            s_ranging.restart_pending = false;
            workflow_service_ranging_done(false, false, "start_failed:ESP_ERR_INVALID_STATE");
        }
        return;
    }
    const bool orphaned = s_ranging.orphaned || s_core.step != WORKFLOW_STEP_RANGING;
    const bool restart = s_ranging.restart_pending && s_core.step == WORKFLOW_STEP_RANGING;
    memset(&s_ranging, 0, sizeof(s_ranging));
    if (orphaned) {
        (void)nrf_topology_service_invalidate_ranging();
        if (restart) workflow_service_ranging_start();
        return;
    }
    if (workflow_service_ranging_save(progress) != ESP_OK) {
        (void)nrf_topology_service_invalidate_ranging();
        workflow_service_ranging_done(false, false, "ranging_save_failed");
        return;
    }
    if (progress->pair_count == 0U && !progress->success) (void)nrf_topology_service_invalidate_ranging();
    workflow_service_ranging_done(progress->success, progress->partial, progress->failure_reason);
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
    workflow_placement_reason_t blocker,
    int64_t height_rel_main_mm,
    uint64_t sample_ms,
    uint64_t now_ms,
    bool collect_sample,
    workflow_placement_reason_t *reason,
    int32_t *range_mm)
{
    *reason = WORKFLOW_PLACEMENT_REASON_COLLECTING;
    *range_mm = -1;
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
        if (blocker != WORKFLOW_PLACEMENT_REASON_STABLE) {
            *reason = blocker;
        } else if (!vertical) {
            *reason = WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL;
        } else if (!height_valid) {
            *reason = WORKFLOW_PLACEMENT_REASON_MAIN_REFERENCE;
        } else {
            *reason = WORKFLOW_PLACEMENT_REASON_CLOCK_BACKWARD;
        }
        return false;
    }
    if (now_ms - sample_ms > WORKFLOW_PLACEMENT_FRESH_MS) {
        workflow_service_placement_clear_samples(history);
        *reason = WORKFLOW_PLACEMENT_REASON_SAMPLE_STALE;
        return false;
    }
    if (now_ms < history->blocked_until_ms) {
        *reason = WORKFLOW_PLACEMENT_REASON_OVERFLOW_BLOCKED;
        return false;
    }

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
            /* ponytail: the cap bounds RAM; overflow blocks one window rather than lose a live outlier. Raise it if polling gets faster than the cap/window ratio. */
            workflow_service_placement_clear_samples(history);
            history->blocked_until_ms = now_ms + WORKFLOW_PLACEMENT_WINDOW_MS;
            *reason = WORKFLOW_PLACEMENT_REASON_OVERFLOW_BLOCKED;
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
        *reason = WORKFLOW_PLACEMENT_REASON_CLOCK_BACKWARD;
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
    const int64_t spread = maximum - minimum;
    *range_mm = spread > INT32_MAX ? INT32_MAX : (int32_t)spread;
    if (spread > WORKFLOW_PLACEMENT_MAX_RANGE_MM) {
        *reason = WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED;
        return false;
    }
    *reason = WORKFLOW_PLACEMENT_REASON_STABLE;
    return true;
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

static bool workflow_service_tracks_placement(workflow_step_t step)
{
    return step == WORKFLOW_STEP_PLACEMENT ||
        step == WORKFLOW_STEP_GPS_CHOICE ||
        step == WORKFLOW_STEP_GPS_PENDING ||
        step == WORKFLOW_STEP_LAYOUT_READY;
}

/* Logs a node's stability state at once when its reason changes and otherwise once per log period. */
static void workflow_service_placement_log_node(
    workflow_placement_history_t *history,
    const workflow_placement_node_t *node,
    int32_t age_ms,
    uint32_t pressure_pa,
    bool periodic_due)
{
    const bool changed = !history->reason_logged || history->logged_reason != node->reason;
    if (!changed && !periodic_due) return;
    history->reason_logged = true;
    history->logged_reason = node->reason;
    ESP_LOGI(TAG,
        "PLACEMENT_STABILITY node=%s stable=%u reason=%s trigger=%s samples=%u/%u range_mm=%d limit_mm=%d "
        "age_ms=%d vertical=%u height_valid=%u height_mm=%d pressure_pa=%u",
        node->node,
        node->stable ? 1U : 0U,
        workflow_core_placement_reason_name(node->reason),
        changed ? "change" : "periodic",
        (unsigned)node->sample_count,
        (unsigned)WORKFLOW_PLACEMENT_MIN_SAMPLES,
        (int)node->range_mm,
        (int)WORKFLOW_PLACEMENT_MAX_RANGE_MM,
        (int)age_ms,
        node->vertical ? 1U : 0U,
        node->height_rel_main_mm_valid ? 1U : 0U,
        (int)node->height_rel_main_mm,
        (unsigned)pressure_pa);
}

static void workflow_service_placement_poll(bool collect_samples)
{
    workflow_placement_snapshot_t *status = &s_placement.status;
    memset(status, 0, sizeof(*status));
    const uint64_t now_ms = workflow_service_now_ms();
    const bool tracks_placement = workflow_service_tracks_placement(s_core.step);
    device_status_snapshot_t *device = &s_poll_scratch.device;
    bool device_valid = device_status_try_get_snapshot(device) == ESP_OK;
    if (device_valid) {
        s_placement.main_cache = device->main;
        s_placement.main_cached = true;
        s_placement.main_cached_ms = now_ms;
    } else if (s_placement.main_cached && now_ms >= s_placement.main_cached_ms &&
               now_ms - s_placement.main_cached_ms <= WORKFLOW_PLACEMENT_MAIN_HOLD_MS) {
        device->main = s_placement.main_cache;
        device_valid = true;
    }
    const bool main_reference_valid = device_valid &&
        device->main.height_reference.state == DEVICE_MAIN_HEIGHT_REFERENCE_VALID &&
        device->main.height_reference.generation != 0U;
    const bool main_baro_valid = device_valid && device->main.bmp280_valid &&
        device->main.pressure_pa > 0U;
    const bool main_height_valid = main_reference_valid && main_baro_valid;
    const uint16_t main_generation = main_reference_valid
        ? device->main.height_reference.generation
        : 0U;
    const workflow_placement_reason_t main_blocker = !device_valid
        ? WORKFLOW_PLACEMENT_REASON_DEVICE_STATUS_BUSY
        : !main_reference_valid ? WORKFLOW_PLACEMENT_REASON_MAIN_REFERENCE
        : !main_baro_valid ? WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER
        : WORKFLOW_PLACEMENT_REASON_STABLE;
    const bool take_samples = tracks_placement && collect_samples;
    const bool log_due = take_samples && now_ms >= s_placement.next_log_ms;

    if (!tracks_placement) {
        for (size_t i = 0U; i < WORKFLOW_MAX_CALIBRATION_NODES; ++i) {
            workflow_service_placement_reset_history(&s_placement.history[i]);
        }
        s_placement.next_log_ms = 0U;
    }

    workflow_placement_node_t *main_node = &status->per_node[status->per_node_count++];
    workflow_service_placement_set_name(main_node, NRF_TOPOLOGY_MAIN_NODE_ID);
    main_node->vertical = true;
    main_node->height_rel_main_mm_valid = main_height_valid;
    main_node->height_rel_main_mm = 0;
    main_node->altitude_asl_m_valid = main_baro_valid;
    if (main_baro_valid) {
        main_node->altitude_asl_m =
            (double)nrf_baro_pressure_to_altitude_mm(device->main.pressure_pa) / 1000.0;
    }
    main_node->stable = workflow_service_placement_update_node(
        &s_placement.history[0],
        NRF_TOPOLOGY_MAIN_NODE_ID,
        main_generation,
        main_node->vertical,
        main_height_valid,
        main_blocker,
        0,
        now_ms,
        now_ms,
        take_samples,
        &main_node->reason,
        &main_node->range_mm);
    main_node->sample_count = (uint8_t)s_placement.history[0].sample_count;
    if (take_samples) {
        workflow_service_placement_log_node(
            &s_placement.history[0], main_node, 0,
            device_valid ? device->main.pressure_pa : 0U, log_due);
    }

    nrf_topology_snapshot_t *topology = &s_poll_scratch.topology;
    const bool topology_valid = nrf_topology_service_get_snapshot(topology) == ESP_OK;
    bool assigned[NRF_TOPOLOGY_REMOTE_COUNT] = { false };
    bool all_stable = topology_valid && tracks_placement && main_node->stable;
    if (topology_valid) {
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            const nrf_topology_node_snapshot_t *remote = &topology->remotes[channel];
            if (remote->state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
            assigned[channel] = true;
            workflow_placement_node_t *node =
                &status->per_node[status->per_node_count++];
            workflow_service_placement_set_name(node, remote->node_id);

            nrf_remote_sensor_cache_t *cache = &s_poll_scratch.remote_sensor;
            memset(cache, 0, sizeof(*cache));
            const bool cache_read =
                nrf_topology_service_get_sensor_snapshot(channel, cache) == ESP_OK;
            const bool cache_current = cache_read && cache->identity_valid &&
                cache->topology_current && cache->response_valid &&
                cache->identity.node_id == remote->node_id &&
                now_ms >= cache->esp_receipt_uptime_ms &&
                now_ms - cache->esp_receipt_uptime_ms <= WORKFLOW_PLACEMENT_FRESH_MS;
            const bool environment_valid = cache_current &&
                (cache->sensor.valid_mask & NRF_UPSTREAM_SENSOR_VALID_ENV) != 0U &&
                cache->sensor.pressure_pa > 0U;
            node->vertical = cache_current &&
                cache->sensor.orientation_state == NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
            const bool reference_matches = cache_current &&
                cache->sensor.reference_state == NRF_UPSTREAM_REFERENCE_VALID &&
                cache->sensor.reference_generation == main_generation;
            node->height_rel_main_mm_valid = main_height_valid && environment_valid &&
                reference_matches;
            if (node->height_rel_main_mm_valid) {
                node->height_rel_main_mm =
                    (int64_t)cache->sensor.relative_z_mm -
                    (int64_t)device->main.height_reference.relative_z_mm;
            }
            node->altitude_asl_m_valid = environment_valid;
            if (environment_valid) {
                node->altitude_asl_m = (double)cache->sensor.altitude_mm / 1000.0;
            }
            const workflow_placement_reason_t blocker = !cache_current
                ? WORKFLOW_PLACEMENT_REASON_NODE_STALE
                : !node->vertical ? WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL
                : main_blocker != WORKFLOW_PLACEMENT_REASON_STABLE ? main_blocker
                : !environment_valid ? WORKFLOW_PLACEMENT_REASON_NODE_ENVIRONMENT
                : !reference_matches ? WORKFLOW_PLACEMENT_REASON_NODE_REFERENCE
                : WORKFLOW_PLACEMENT_REASON_STABLE;
            workflow_placement_history_t *history = &s_placement.history[(size_t)channel + 1U];
            node->stable = workflow_service_placement_update_node(
                history,
                remote->node_id,
                node->height_rel_main_mm_valid ? main_generation : 0U,
                node->vertical,
                node->height_rel_main_mm_valid,
                blocker,
                node->height_rel_main_mm,
                cache->esp_receipt_uptime_ms,
                now_ms,
                take_samples && cache_current,
                &node->reason,
                &node->range_mm);
            node->sample_count = (uint8_t)history->sample_count;
            if (take_samples) {
                const uint64_t age = now_ms - cache->esp_receipt_uptime_ms;
                const int32_t age_ms = cache_read && now_ms >= cache->esp_receipt_uptime_ms
                    ? (int32_t)(age > INT32_MAX ? INT32_MAX : age)
                    : -1;
                workflow_service_placement_log_node(
                    history, node, age_ms, cache_read ? cache->sensor.pressure_pa : 0U, log_due);
            }
            all_stable = all_stable && node->stable;
        }
    }
    for (size_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (!assigned[channel]) {
            workflow_service_placement_reset_history(&s_placement.history[channel + 1U]);
        }
    }
    if (log_due) s_placement.next_log_ms = now_ms + WORKFLOW_PLACEMENT_LOG_PERIOD_MS;
    status->all_stable = all_stable;
}

static uint8_t workflow_service_assigned_remote_count(void)
{
    nrf_topology_snapshot_t *topology = &s_poll_scratch.topology;
    if (nrf_topology_service_get_snapshot(topology) != ESP_OK) return 0U;

    size_t count = 0U;
    for (size_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        if (topology->remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED) ++count;
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
        if (s_core.step == WORKFLOW_STEP_CALCULATING) {
            workflow_service_mark_calculation_orphaned_locked();
        }
        (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
        s_session_id[0] = '\0';
        s_has_scan = false;
        s_scan_storage_available = false;
        s_calc_settings_ready = false;
        s_scan_cache_valid = false;
        s_calc_settings_cache_valid = false;
        s_calc_velocity_range_set = false;
        s_calc_velocity_range_min_mps = 0.0f;
        s_calc_velocity_range_max_mps = 0.0f;
        workflow_core_reset_object(&s_core, active_object_present, true);
    }

    s_assigned_remotes = workflow_service_assigned_remote_count();
    if (measurement_err != ESP_OK) {
        workflow_service_update_session_locked(measurement_err, NULL);
        workflow_service_step_changed(previous_step, s_core.step);
        return;
    }

    if (measurement.state == MEASUREMENT_SERVICE_LIVE) {
        s_recorder_reconciliation_armed = true;
        workflow_service_update_session_locked(measurement_err, &measurement);
        workflow_service_step_changed(previous_step, s_core.step);
        return;
    }
    if (!s_recorder_reconciliation_armed) {
        workflow_service_update_session_locked(measurement_err, &measurement);
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
    workflow_service_update_session_locked(measurement_err, &measurement);
    workflow_service_step_changed(previous_step, s_core.step);
}

static void workflow_service_mark_calculation_orphaned_locked(void)
{
    if (s_calculation_job_id != 0U) s_orphaned_job_id = s_calculation_job_id;
    s_calculation_job_id = 0U;
}

static void workflow_service_calculation_done_locked(
    bool ok,
    workflow_calculation_class_t calculation_class,
    const char *reason
)
{
    const workflow_step_t previous_step = s_core.step;
    const workflow_command_params_t params = {
        .has_ok = true,
        .ok = ok,
        .reason = reason,
        .calculation_class = calculation_class,
    };
    if (workflow_core_apply(
            &s_core, WORKFLOW_COMMAND_CALCULATION_DONE, &params,
            s_assigned_remotes, s_placement_ready) == WORKFLOW_COMMAND_RESULT_OK) {
        s_calculation_job_id = 0U;
        workflow_service_step_changed(previous_step, s_core.step);
    }
}

static void workflow_service_calculation_start_locked(void)
{
    localization_job_status_t *status = &s_poll_scratch.job_status;
    localization_job_get_status(status);
    if (status->state == LOCALIZATION_JOB_QUEUED || status->state == LOCALIZATION_JOB_RUNNING) {
        workflow_service_calculation_done_locked(
            false, WORKFLOW_CALCULATION_CLASS_INTERNAL, "busy");
        return;
    }
    if (s_orphaned_job_id != 0U && status->job_id == s_orphaned_job_id) {
        s_orphaned_job_id = 0U;
    }

    char *json = NULL;
    uint32_t budget_ms = 0U;
    esp_err_t err = s_active_object_id[0] == '\0'
        ? ESP_ERR_NOT_FOUND
        : object_store_get_calc_settings_json_cached(s_active_object_id, &json);
    if (err == ESP_OK && json != NULL) {
        err = workflow_service_parse_calc_settings_locked(
            json, &budget_ms, &s_poll_scratch.calculation_config, NULL, NULL);
    }
    free(json);
    if (err != ESP_OK) {
        workflow_service_calculation_done_locked(
            false, WORKFLOW_CALCULATION_CLASS_INTERNAL, "calc_settings_invalid");
        return;
    }

    uint32_t job_id = 0U;
    err = localization_job_submit(
        s_active_object_id,
        s_session_id,
        &s_poll_scratch.calculation_config,
        budget_ms,
        &job_id);
    if (err != ESP_OK) {
        char reason[WORKFLOW_REASON_MAX];
        (void)snprintf(reason, sizeof(reason), "submit_failed:%s", esp_err_to_name(err));
        workflow_service_calculation_done_locked(
            false, WORKFLOW_CALCULATION_CLASS_INTERNAL, reason);
        return;
    }
    s_calculation_job_id = job_id;
    if (!s_poll_first_calculation_seen) {
        s_poll_first_calculation_seen = true;
        atomic_store_explicit(&s_poll_hwm_after_first_calculation, true, memory_order_release);
    }
    (void)workflow_core_set_progress(&s_core, 0);
}

static void workflow_service_calculation_poll_locked(void)
{
    if (s_calculation_job_id == 0U && s_orphaned_job_id == 0U) return;
    localization_job_status_t *status = &s_poll_scratch.job_status;
    localization_job_get_status(status);

    if (s_orphaned_job_id != 0U && status->job_id == s_orphaned_job_id) {
        if (status->state == LOCALIZATION_JOB_QUEUED || status->state == LOCALIZATION_JOB_RUNNING) return;
        s_orphaned_job_id = 0U;
    } else if (s_orphaned_job_id != 0U) {
        s_orphaned_job_id = 0U;
    }

    if (s_calculation_job_id == 0U || s_core.step != WORKFLOW_STEP_CALCULATING ||
        status->job_id != s_calculation_job_id ||
        strcmp(status->object_id, s_active_object_id) != 0 ||
        strcmp(status->session_id, s_session_id) != 0) {
        return;
    }
    if (status->state == LOCALIZATION_JOB_QUEUED || status->state == LOCALIZATION_JOB_RUNNING) {
        (void)workflow_core_set_progress(&s_core, (int)status->progress_percent);
        return;
    }
    if (status->state != LOCALIZATION_JOB_SUCCEEDED && status->state != LOCALIZATION_JOB_FAILED) return;

    const localization_recommendation_t *recommendation = &status->summary.recommendation;
    const bool ok = status->state == LOCALIZATION_JOB_SUCCEEDED && recommendation->successful;
    workflow_calculation_class_t calculation_class = WORKFLOW_CALCULATION_CLASS_NONE;
    const char *reason = recommendation->reason_summary;
    if (!ok) {
        switch (recommendation->action) {
            case LOCALIZATION_RECOMMENDATION_RECORD_AGAIN:
                calculation_class = WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN;
                break;
            case LOCALIZATION_RECOMMENDATION_MOVE_SENSORS:
                calculation_class = WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS;
                break;
            case LOCALIZATION_RECOMMENDATION_NONE:
            default:
                calculation_class = WORKFLOW_CALCULATION_CLASS_INTERNAL;
                break;
        }
        if (status->state == LOCALIZATION_JOB_FAILED && status->error != ESP_OK) {
            reason = esp_err_to_name(status->error);
        }
        if (reason == NULL || reason[0] == '\0') reason = "calculation_failed";
    }
    workflow_service_calculation_done_locked(ok, calculation_class, reason);
}

esp_err_t workflow_service_init(void)
{
    if (s_initialized) return ESP_OK;
    if (s_poll_task != NULL || s_poll_join_pending) return ESP_ERR_INVALID_STATE;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    s_async_enqueue_mutex = xSemaphoreCreateMutex();
    s_poll_stop_ack = xSemaphoreCreateBinary();
    s_async_queue = xQueueCreate(WORKFLOW_ASYNC_QUEUE_DEPTH, sizeof(workflow_async_command_t));
    if (s_async_enqueue_mutex == NULL || s_poll_stop_ack == NULL || s_async_queue == NULL) {
        if (s_async_queue != NULL) vQueueDelete(s_async_queue);
        if (s_async_enqueue_mutex != NULL) vSemaphoreDelete(s_async_enqueue_mutex);
        if (s_poll_stop_ack != NULL) vSemaphoreDelete(s_poll_stop_ack);
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        s_async_enqueue_mutex = NULL;
        s_poll_stop_ack = NULL;
        s_async_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    char object_id[OBJECT_SERVICE_OBJECT_ID_SIZE] = {0};
    bool active_object_present = false;
    (void)workflow_service_active_object(object_id, &active_object_present);
    (void)snprintf(s_active_object_id, sizeof(s_active_object_id), "%s", object_id);
    workflow_core_init(&s_core, active_object_present);
    if (workflow_service_lock()) {
        s_assigned_remotes = workflow_service_assigned_remote_count();
        workflow_service_unlock();
    }
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;
    s_session_id[0] = '\0';
    memset(&s_last_async, 0, sizeof(s_last_async));
    memset(&s_last_material_save, 0, sizeof(s_last_material_save));
    s_placement_ready = false;
    s_has_scan = false;
    s_scan_storage_available = false;
    s_scan_cache_valid = false;
    s_scan_cache_updated_ms = 0U;
    s_scan_cache_step = WORKFLOW_STEP_COUNT;
    s_scan_cache_object_id[0] = '\0';
    s_scan_cache_session_id[0] = '\0';
    s_calc_settings_ready = false;
    s_calc_settings_cache_valid = false;
    s_calc_settings_cache_updated_ms = 0U;
    s_calc_settings_cache_object_id[0] = '\0';
    s_calc_material[0] = '\0';
    s_calc_velocity_range_set = false;
    s_calc_velocity_range_min_mps = 0.0f;
    s_calc_velocity_range_max_mps = 0.0f;
    s_calculation_job_id = 0U;
    s_orphaned_job_id = 0U;
    atomic_store_explicit(&s_poll_hwm_after_first_calculation, false, memory_order_relaxed);
    atomic_store_explicit(&s_poll_missed_deadlines, 0U, memory_order_relaxed);
    s_poll_first_calculation_seen = false;
    s_poll_task = NULL;
    atomic_store_explicit(&s_poll_stop_requested, false, memory_order_release);
    s_poll_join_pending = false;
    s_async_queue_occupancy = 0U;
    memset(&s_async_queue_stats, 0, sizeof(s_async_queue_stats));
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
    if (localization_job_init() != ESP_OK) {
        workflow_service_shutdown();
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    const BaseType_t task_created = workflow_service_create_poll_task(&s_poll_task);
    if (task_created != pdPASS) {
        s_poll_task = NULL;
        workflow_service_shutdown();
        return ESP_ERR_NO_MEM;
    }
    s_poll_join_pending = true;
    return ESP_OK;
}

esp_err_t workflow_service_shutdown(void)
{
    s_initialized = false;
    if (s_poll_join_pending) {
        atomic_store_explicit(&s_poll_stop_requested, true, memory_order_release);
        if (
            s_poll_stop_ack == NULL ||
            xSemaphoreTake(
                s_poll_stop_ack,
                pdMS_TO_TICKS(APP_STOP_TIMEOUT_WORKER_MS)
            ) != pdTRUE
        ) {
            return ESP_ERR_TIMEOUT;
        }
        s_poll_join_pending = false;
    }
    if (s_async_queue != NULL) {
        vQueueDelete(s_async_queue);
        s_async_queue = NULL;
    }
    if (s_async_enqueue_mutex != NULL) {
        vSemaphoreDelete(s_async_enqueue_mutex);
        s_async_enqueue_mutex = NULL;
    }
    if (s_poll_stop_ack != NULL) {
        vSemaphoreDelete(s_poll_stop_ack);
        s_poll_stop_ack = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    s_recorder_reconciliation_armed = false;
    s_assigned_remotes = 0U;
    s_active_object_id[0] = '\0';
    s_session_id[0] = '\0';
    s_calculation_job_id = 0U;
    s_orphaned_job_id = 0U;
    s_scan_cache_valid = false;
    s_scan_storage_available = false;
    s_calc_settings_cache_valid = false;
    s_calc_material[0] = '\0';
    s_calc_velocity_range_set = false;
    s_calc_velocity_range_min_mps = 0.0f;
    s_calc_velocity_range_max_mps = 0.0f;
    atomic_store_explicit(&s_poll_hwm_after_first_calculation, false, memory_order_relaxed);
    s_poll_first_calculation_seen = false;
    s_placement_ready_predicate = NULL;
    s_placement_ready_context = NULL;
    memset(&s_calibration, 0, sizeof(s_calibration));
    memset(&s_placement, 0, sizeof(s_placement));
    memset(&s_ranging, 0, sizeof(s_ranging));
    memset(&s_core, 0, sizeof(s_core));
    return ESP_OK;
}

static void workflow_service_snapshot_locked(workflow_snapshot_t *snapshot)
{
    workflow_core_snapshot(
        &s_core, s_assigned_remotes, s_placement_ready, snapshot);
    if (!s_scan_storage_available || s_active_object_id[0] == '\0') {
        snapshot->allowed_actions &= ~WORKFLOW_ACTION(WORKFLOW_COMMAND_SCAN_START);
    }
    workflow_service_copy_string(snapshot->object_id, sizeof(snapshot->object_id), s_active_object_id);
    workflow_service_copy_string(snapshot->session_id, sizeof(snapshot->session_id), s_session_id);
    snapshot->has_scan = s_has_scan;
    snapshot->calc_settings_ready = s_calc_settings_ready;
    workflow_service_copy_string(snapshot->calc_material, sizeof(snapshot->calc_material), s_calc_material);
    snapshot->calc_velocity_range_set = s_calc_velocity_range_set;
    snapshot->calc_velocity_range_min_mps = s_calc_velocity_range_min_mps;
    snapshot->calc_velocity_range_max_mps = s_calc_velocity_range_max_mps;
    snapshot->last_async = s_last_async;
    snapshot->last_material_save = s_last_material_save;
    snapshot->calibration = s_calibration.status;
    snapshot->placement = s_placement.status;
}

/*
 * Poll waits indefinitely for the workflow mutex; measurement, object-service,
 * topology/calibration/ranging, and localization-job getters may also wait on
 * their service mutexes. Device-status reads are try-only. Presence refresh
 * checks scan presence and uses the object store's cached settings snapshot;
 * settings file reads occur on a cold cache or generation change. The task is
 * low priority and the LVGL path uses try_get_snapshot(), never this poll.
 */
esp_err_t workflow_service_poll(void)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    workflow_service_process_async_locked();
    workflow_service_reconcile_locked();
    workflow_service_calibration_poll();
    workflow_service_ranging_poll();
    workflow_service_placement_poll(true);
    s_placement_ready = workflow_service_placement_ready();
    workflow_service_refresh_presence_locked();
    workflow_service_calculation_poll_locked();
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_get_snapshot(workflow_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    const esp_err_t poll_err = workflow_service_poll();
    if (poll_err != ESP_OK) return poll_err;
    if (!workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    workflow_service_snapshot_locked(snapshot);
    workflow_service_unlock();
    return ESP_OK;
}

esp_err_t workflow_service_try_get_snapshot(workflow_snapshot_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, 0U) != pdTRUE) return ESP_ERR_TIMEOUT;
    workflow_service_snapshot_locked(out);
    workflow_service_unlock();
    return ESP_OK;
}


esp_err_t workflow_service_get_queue_stats(workflow_service_queue_stats_t *stats)
{
    if (stats == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized || s_async_enqueue_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_async_enqueue_mutex, 0U) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *stats = s_async_queue_stats;
    xSemaphoreGive(s_async_enqueue_mutex);
    return ESP_OK;
}

static workflow_command_result_t workflow_service_command_locked(
    workflow_command_t command,
    const workflow_command_params_t *params
)
{
    workflow_service_reconcile_locked();
    workflow_service_placement_poll(false);
    s_placement_ready = workflow_service_placement_ready();

    workflow_command_params_t applied_params = *params;
    if (command == WORKFLOW_COMMAND_SET_DISTANCE) {
        if (!params->has_generation || !params->has_distance || params->distance_mm <= 0 ||
            params->node_a == params->node_b) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        if (params->expected_generation != s_core.generation) return WORKFLOW_COMMAND_RESULT_STALE;
        if (s_core.step != WORKFLOW_STEP_RANGING_RESULT || s_active_object_id[0] == '\0')
            return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
        uint32_t limits[NRF_TOPOLOGY_REMOTE_COUNT];
        esp_err_t err = workflow_service_ranging_load_limits(limits);
        if (err == ESP_OK) err = nrf_topology_service_ranging_set_manual(
            params->ranging_started_at_ms, params->ranging_updated_at_ms,
            params->node_a, params->node_b, params->distance_mm, limits);
        if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_FOUND) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
        if (err == ESP_ERR_INVALID_STATE) return WORKFLOW_COMMAND_RESULT_STALE;
        if (err != ESP_OK) return WORKFLOW_COMMAND_RESULT_UNAVAILABLE;
        nrf_topology_ranging_progress_t *progress = &s_poll_scratch.ranging_progress;
        err = nrf_topology_service_ranging_get_progress(progress);
        if (err == ESP_OK) err = workflow_service_ranging_save(progress);
        if (err != ESP_OK) {
            (void)nrf_topology_service_invalidate_ranging();
            applied_params.has_ok = true;
            applied_params.ok = false;
            applied_params.reason = "ranging_save_failed";
            (void)workflow_core_apply(&s_core, command, &applied_params, s_assigned_remotes, s_placement_ready);
            return WORKFLOW_COMMAND_RESULT_UNAVAILABLE;
        }
        applied_params.has_ok = true;
        applied_params.ok = progress->success;
        applied_params.reason = progress->failure_reason;
    }
    if (command == WORKFLOW_COMMAND_SCAN_START || command == WORKFLOW_COMMAND_SCAN_COMPLETE) {
        s_scan_cache_valid = false;
        workflow_service_refresh_presence_locked();
        if (s_active_object_id[0] == '\0' || !s_scan_storage_available ||
            (command == WORKFLOW_COMMAND_SCAN_COMPLETE && !s_has_scan)) {
            return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
        }
        if (command == WORKFLOW_COMMAND_SCAN_COMPLETE) {
            applied_params.has_ok = true;
            applied_params.ok = true;
        }
    }
    if ((command == WORKFLOW_COMMAND_GPS_ACCEPT ||
         command == WORKFLOW_COMMAND_GPS_CAPTURE_FAILED) && !params->has_generation) {
        return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    }
    if (command == WORKFLOW_COMMAND_GPS_ACCEPT && s_active_object_id[0] == '\0') {
        return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    }

    const workflow_core_t previous = s_core;
    char previous_session[SEISMIC_RECORDER_SESSION_ID_SIZE];
    memcpy(previous_session, s_session_id, sizeof(previous_session));
    const workflow_command_result_t result = workflow_core_apply(
        &s_core, command, &applied_params, s_assigned_remotes, s_placement_ready);
    if (result != WORKFLOW_COMMAND_RESULT_OK) return result;

    if (command == WORKFLOW_COMMAND_GPS_ACCEPT) {
        object_store_location_t location = {0};
        location.valid = true;
        location.latitude = params->latitude;
        location.longitude = params->longitude;
        location.accuracy_m = (float)params->accuracy_m;
        location.altitude_valid = params->altitude_valid;
        location.altitude_m = params->altitude_m;
        location.altitude_accuracy_m = (float)params->altitude_accuracy_m;
        location.captured_at_ms = params->captured_at_ms;
        workflow_service_copy_string(location.source, sizeof(location.source), "PHONE_GPS");
        if (object_store_set_location(s_active_object_id, &location) != ESP_OK) {
            s_core = previous;
            return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
        }
    }

    if (s_core.step != previous.step) {
        workflow_service_placement_poll(false);
        workflow_service_step_changed(previous.step, s_core.step);
        if (!workflow_service_step_has_session(s_core.step)) {
            workflow_service_update_session_locked(ESP_ERR_INVALID_STATE, NULL);
        }
    }
    if (previous.step == WORKFLOW_STEP_CALCULATING && s_core.step != WORKFLOW_STEP_CALCULATING) {
        workflow_service_mark_calculation_orphaned_locked();
    }

    if (s_core.step == WORKFLOW_STEP_CALIBRATING && previous.step != WORKFLOW_STEP_CALIBRATING) {
        if (workflow_service_calibration_start() != ESP_OK) {
            s_core = previous;
            memcpy(s_session_id, previous_session, sizeof(s_session_id));
            return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
        }
    } else if (previous.step == WORKFLOW_STEP_CALIBRATING &&
               s_core.step != WORKFLOW_STEP_CALIBRATING) {
        workflow_service_calibration_cancel();
    }

    esp_err_t measurement_err = ESP_OK;
    if (command == WORKFLOW_COMMAND_START_RECORDING) {
        s_session_id[0] = '\0';
        s_scan_cache_valid = false;
        measurement_err = measurement_service_start_async();
        if (measurement_err == ESP_OK) s_recorder_reconciliation_armed = true;
    } else if (command == WORKFLOW_COMMAND_STOP_RECORDING) {
        measurement_err = measurement_service_stop_async();
    } else if (command == WORKFLOW_COMMAND_DELETE_RECORDING) {
        measurement_err = measurement_service_delete_async();
    }
    if (measurement_err != ESP_OK) {
        s_core = previous;
        memcpy(s_session_id, previous_session, sizeof(s_session_id));
        return WORKFLOW_COMMAND_RESULT_NOT_ALLOWED;
    }

    if (command == WORKFLOW_COMMAND_CALCULATE_SOURCES && s_core.step == WORKFLOW_STEP_CALCULATING) {
        workflow_service_calculation_start_locked();
    }
    return WORKFLOW_COMMAND_RESULT_OK;
}

static void workflow_service_process_async_locked(void)
{
    workflow_async_command_t item;
    while (workflow_service_receive_async(&item) == pdTRUE) {
        if (item.kind == WORKFLOW_ASYNC_MATERIAL_SAVE) {
            workflow_service_reconcile_locked();
            esp_err_t result = ESP_ERR_NOT_FOUND;
            if (s_active_object_id[0] != '\0') {
                const localization_material_preset_t *preset =
                    localization_material_preset_lookup(item.material_id);
                const bool custom = strcmp(item.material_id, "custom") == 0;
                if (custom || preset != NULL) {
                    uint32_t budget_ms = 0U;
                    result = localization_config_standard_processing(
                        &s_poll_scratch.calculation_config, &budget_ms);
                    if (result == ESP_OK) {
                        s_poll_scratch.calculation_config.propagation_speed_mps = custom
                            ? (double)item.custom_v_mps : (double)preset->propagation_speed_mps;
                        s_poll_scratch.calculation_config.propagation_speed_uncertainty_mps = custom
                            ? (double)item.custom_sigma_mps
                            : (double)preset->propagation_speed_uncertainty_mps;
                        if (item.has_range) {
                            s_poll_scratch.calculation_config.velocity_range.bounds =
                                (localization_velocity_bounds_t) {
                                    .v_min_mps = (float)item.range_min_mps,
                                    .v_max_mps = (float)item.range_max_mps,
                                    .vp_vs_min = 0.0f,
                                    .vp_vs_max = 0.0f,
                                    .set = true,
                                };
                            s_poll_scratch.calculation_config.velocity_range.source =
                                LOCALIZATION_VELOCITY_RANGE_SOURCE_USER;
                        }
                        char *json = NULL;
                        result = localization_config_to_json(
                            &s_poll_scratch.calculation_config, item.material_id, budget_ms, &json);
                        if (result == ESP_OK && custom && !item.has_range) {
                            cJSON *settings = cJSON_Parse(json);
                            if (settings == NULL) {
                                result = ESP_ERR_INVALID_RESPONSE;
                            } else {
                                cJSON_DeleteItemFromObjectCaseSensitive(settings, "velocity_range");
                                char *without_range = cJSON_PrintUnformatted(settings);
                                cJSON_Delete(settings);
                                if (without_range == NULL) {
                                    result = ESP_ERR_NO_MEM;
                                } else {
                                    cJSON_free(json);
                                    json = without_range;
                                }
                            }
                        }
                        if (result == ESP_OK) {
                            result = object_store_set_calc_settings_json(s_active_object_id, json);
                        }
                        cJSON_free(json);
                    }
                } else {
                    result = ESP_ERR_INVALID_ARG;
                }
            }
            s_last_material_save = (workflow_last_material_save_t) {
                .seq = item.seq,
                .result = result,
            };
            if (result == ESP_OK) {
                s_calc_settings_cache_valid = false;
                workflow_service_refresh_calc_settings_locked(workflow_service_now_ms());
            }
            continue;
        }
        const workflow_command_params_t params = {
            .has_generation = item.has_generation,
            .expected_generation = item.expected_generation,
            .has_distance = item.distance.has_distance,
            .node_a = item.distance.node_a, .node_b = item.distance.node_b,
            .distance_mm = item.distance.distance_mm,
            .ranging_started_at_ms = item.distance.ranging_started_at_ms,
            .ranging_updated_at_ms = item.distance.ranging_updated_at_ms,
        };
        s_last_async = (workflow_last_async_t){
            .seq = item.seq,
            .command = item.command,
            .result = workflow_service_command_locked(item.command, &params),
        };
        ESP_LOGI(TAG, "WORKFLOW_COMMAND_DONE seq=%lu name=%s result=%d expected_gen=%lu gen=%lu step=%s remotes=%u",
            (unsigned long)item.seq, workflow_core_command_name(item.command),
            (int)s_last_async.result, (unsigned long)item.expected_generation,
            (unsigned long)s_core.generation, workflow_core_step_name(s_core.step),
            (unsigned)s_assigned_remotes);
    }
}

workflow_command_result_t workflow_service_command(
    workflow_command_t command,
    const workflow_command_params_t *params
)
{
    if (params == NULL) return WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT;
    if (!s_initialized || !workflow_service_lock()) return WORKFLOW_COMMAND_RESULT_UNAVAILABLE;
    const workflow_command_result_t result = workflow_service_command_locked(command, params);
    workflow_service_unlock();
    return result;
}

static esp_err_t workflow_service_enqueue_command(
    workflow_command_t command,
    const workflow_command_params_t *params,
    uint32_t *out_seq
)
{
    if (command < 0 || command >= WORKFLOW_COMMAND_COUNT || out_seq == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (command == WORKFLOW_COMMAND_CALIBRATION_DONE ||
        command == WORKFLOW_COMMAND_RANGING_DONE ||
        command == WORKFLOW_COMMAND_CALCULATION_DONE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_async_queue == NULL || s_async_enqueue_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_async_enqueue_mutex, 0U) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint32_t seq = s_async_next_seq + 1U;
    if (seq == 0U) seq = 1U;
    workflow_async_command_t item = {
        .seq = seq,
        .kind = WORKFLOW_ASYNC_COMMAND,
        .command = command,
        .has_generation = params->has_generation,
        .expected_generation = params->expected_generation,
        .distance = {
            .has_distance = params->has_distance, .node_a = params->node_a, .node_b = params->node_b,
            .distance_mm = params->distance_mm,
            .ranging_started_at_ms = params->ranging_started_at_ms,
            .ranging_updated_at_ms = params->ranging_updated_at_ms,
        },
    };
    if (xQueueSend(s_async_queue, &item, 0U) != pdTRUE) {
        workflow_service_record_async_enqueue(false);
        xSemaphoreGive(s_async_enqueue_mutex);
        return ESP_ERR_NO_MEM;
    }
    workflow_service_record_async_enqueue(true);
    s_async_next_seq = seq;
    *out_seq = seq;
    xSemaphoreGive(s_async_enqueue_mutex);
    return ESP_OK;
}

esp_err_t workflow_service_command_async(workflow_command_t command, bool has_generation,
    uint32_t expected_generation, uint32_t *out_seq)
{
    if (command == WORKFLOW_COMMAND_SET_DISTANCE) return ESP_ERR_INVALID_ARG;
    const workflow_command_params_t params = {.has_generation = has_generation, .expected_generation = expected_generation};
    return workflow_service_enqueue_command(command, &params, out_seq);
}

esp_err_t workflow_service_set_distance_async(const workflow_command_params_t *params, uint32_t *out_seq)
{
    if (params == NULL || !params->has_generation || !params->has_distance ||
        params->distance_mm <= 0 || params->node_a == params->node_b) return ESP_ERR_INVALID_ARG;
    return workflow_service_enqueue_command(WORKFLOW_COMMAND_SET_DISTANCE, params, out_seq);
}

esp_err_t workflow_service_save_calc_material_range_async(
    const char *material_id,
    uint32_t custom_v_mps,
    uint32_t custom_sigma_mps,
    bool has_range,
    uint32_t range_min_mps,
    uint32_t range_max_mps
)
{
    if (material_id == NULL || strnlen(material_id, WORKFLOW_CALC_MATERIAL_ID_SIZE) >=
        WORKFLOW_CALC_MATERIAL_ID_SIZE) return ESP_ERR_INVALID_ARG;
    const bool custom = strcmp(material_id, "custom") == 0;
    const localization_material_preset_t *preset = custom
        ? NULL : localization_material_preset_lookup(material_id);
    if ((!custom && preset == NULL) ||
        (custom && (!(custom_v_mps > 50U && custom_v_mps < 8000U) ||
            custom_sigma_mps == 0U || custom_sigma_mps >= custom_v_mps)) ||
        (!custom && has_range) ||
        (has_range && (range_min_mps < 50U || range_min_mps > range_max_mps ||
            range_max_mps > 8000U || range_max_mps > 10U * range_min_mps))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_async_queue == NULL || s_async_enqueue_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_async_enqueue_mutex, 0U) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint32_t seq = s_async_next_seq + 1U;
    if (seq == 0U) seq = 1U;
    workflow_async_command_t item = {
        .seq = seq,
        .kind = WORKFLOW_ASYNC_MATERIAL_SAVE,
        .custom_v_mps = custom_v_mps,
        .custom_sigma_mps = custom_sigma_mps,
        .has_range = has_range,
        .range_min_mps = range_min_mps,
        .range_max_mps = range_max_mps,
    };
    (void)snprintf(item.material_id, sizeof(item.material_id), "%s", material_id);
    if (xQueueSend(s_async_queue, &item, 0U) != pdTRUE) {
        workflow_service_record_async_enqueue(false);
        xSemaphoreGive(s_async_enqueue_mutex);
        return ESP_ERR_NO_MEM;
    }
    workflow_service_record_async_enqueue(true);
    s_async_next_seq = seq;
    xSemaphoreGive(s_async_enqueue_mutex);
    return ESP_OK;
}

esp_err_t workflow_service_save_calc_material_async(
    const char *material_id,
    uint32_t custom_v_mps,
    uint32_t custom_sigma_mps
)
{
    return workflow_service_save_calc_material_range_async(
        material_id, custom_v_mps, custom_sigma_mps, false, 0U, 0U);
}

esp_err_t workflow_service_set_placement_ready_predicate(
    workflow_service_placement_ready_fn predicate,
    void *context
)
{
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    s_placement_ready_predicate = predicate;
    s_placement_ready_context = context;
    s_placement_ready = workflow_service_placement_ready();
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

esp_err_t workflow_service_notify_calc_settings_saved(const char *object_id)
{
    if (object_id == NULL || object_id[0] == '\0' || strlen(object_id) >= OBJECT_STORE_ID_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !workflow_service_lock()) return ESP_ERR_INVALID_STATE;
    if (strcmp(object_id, s_active_object_id) == 0) {
        s_calc_settings_cache_valid = false;
        s_calc_settings_ready = false;
        s_calc_velocity_range_set = false;
        s_calc_velocity_range_min_mps = 0.0f;
        s_calc_velocity_range_max_mps = 0.0f;
        workflow_core_set_calc_settings_ready(&s_core, false);
    }
    workflow_service_unlock();
    return ESP_OK;
}

/*
 * This call graph has no cache-disabling SPI-flash writes: settings and scan
 * presence are SD reads; measurement *_async only queues commands (the START
 * guard reserves RAM state, and workflow START supplies no object ID); topology
 * requests and barometer/device snapshots only update or copy RAM; and
 * localization_job_submit queues RAM state for its separate worker task.
 * Object activation writes and NVS/partition/SPI-flash APIs are not reachable,
 * so the fixed task stack can safely stay in PSRAM.
 */
static BaseType_t workflow_service_create_poll_task(TaskHandle_t *task_out)
{
    return xTaskCreatePinnedToCoreWithCaps(
        workflow_service_poll_task,
        "workflow_poll",
        WORKFLOW_SERVICE_POLL_STACK_BYTES,
        NULL,
        WORKFLOW_SERVICE_POLL_TASK_PRIORITY,
        task_out,
        APP_TASK_CORE_SERVICES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
}

static void workflow_service_log_poll_stack_hwm(const char *phase, uint32_t poll_count)
{
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "poll task STACK_HWM phase=%s free=%u of %u bytes polls=%u",
        phase,
        (unsigned)free_bytes,
        (unsigned)WORKFLOW_SERVICE_POLL_STACK_BYTES,
        (unsigned)poll_count);
}

static void workflow_service_poll_task(void *argument)
{
    (void)argument;
    uint32_t poll_count = 0U;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t poll_period_ticks = pdMS_TO_TICKS(WORKFLOW_SERVICE_POLL_PERIOD_MS);
    for (;;) {
        if (atomic_load_explicit(&s_poll_stop_requested, memory_order_acquire)) break;
        (void)workflow_service_poll();
        ++poll_count;
        if (poll_count == 100U) workflow_service_log_poll_stack_hwm("after-100-polls", poll_count);
        if (atomic_exchange_explicit(
                &s_poll_hwm_after_first_calculation, false, memory_order_acq_rel)) {
            workflow_service_log_poll_stack_hwm("after-first-calculation", poll_count);
        }
        if (atomic_load_explicit(&s_poll_stop_requested, memory_order_acquire)) break;
        uint32_t last_wake_tick = (uint32_t)last_wake;
        const uint32_t missed_deadlines = app_tick_skip_missed_periods(
            &last_wake_tick,
            (uint32_t)xTaskGetTickCount(),
            (uint32_t)poll_period_ticks);
        last_wake = (TickType_t)last_wake_tick;
        if (missed_deadlines > 0U) {
            const uint32_t total_missed = (uint32_t)atomic_fetch_add_explicit(
                &s_poll_missed_deadlines,
                missed_deadlines,
                memory_order_relaxed) + missed_deadlines;
            ESP_LOGW(TAG, "poll missed_deadlines=%u total_missed_deadlines=%u",
                (unsigned)missed_deadlines, (unsigned)total_missed);
        }
        vTaskDelayUntil(&last_wake, poll_period_ticks);
    }
    s_poll_task = NULL;
    (void)xSemaphoreGive(s_poll_stop_ack);
    vTaskDelete(NULL);
}

#if defined(WORKFLOW_SERVICE_TEST_HARNESS)
void workflow_service_test_lock_mutex(void)
{
    if (s_mutex != NULL) (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void workflow_service_test_unlock_mutex(void)
{
    if (s_mutex != NULL) (void)xSemaphoreGive(s_mutex);
}

uint32_t workflow_service_test_poll_missed_deadline_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_poll_missed_deadlines, memory_order_relaxed);
}
#endif
