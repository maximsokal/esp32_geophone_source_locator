#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_status.h"
#include "freertos/semphr.h"
#include "main_barometer.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_service.h"
#include "workflow_service.h"
#include "host_rtos.h"
#include "workflow_service_host_fakes.h"

typedef struct { bool held; } fake_mutex_t;

static measurement_service_snapshot_t s_measurement;
static object_service_summary_t s_object;
static nrf_topology_snapshot_t s_topology;
static nrf_remote_sensor_cache_t s_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static nrf_cs_cycle_snapshot_t s_cycle;
static bool s_cycle_valid;
static device_status_snapshot_t s_device;
static esp_err_t s_device_result;
static uint64_t s_now_ms;
static uint16_t s_next_generation;
static uint16_t s_requested_generation;
static uint16_t s_imu_generation;
static uint16_t s_main_generation;
static uint8_t s_requested_mask;
static unsigned s_height_requests;
static unsigned s_imu_requests;
static unsigned s_cs_requests;
static unsigned s_main_requests;
static unsigned s_log_count;
static char s_last_log[256];
static nrf_topology_ranging_progress_t s_ranging_progress;
static esp_err_t s_ranging_start_result;
static esp_err_t s_ranging_progress_result;
static nrf_cs_policy_t s_ranging_policy;
static unsigned s_ranging_start_calls;
static unsigned s_ranging_progress_calls;
static unsigned s_ranging_invalidate_calls;
static unsigned s_ranging_cancel_calls;
static bool s_ranging_cancel_winds_down;
static unsigned s_ranging_call_count;
static char s_ranging_calls[32];

enum {
    RANGING_CALL_INVALIDATE = 'I',
    RANGING_CALL_START = 'S',
    RANGING_CALL_PROGRESS = 'P',
};

static void ranging_call(char call)
{
    assert(s_ranging_call_count < sizeof(s_ranging_calls));
    s_ranging_calls[s_ranging_call_count++] = call;
}

static void clear_ranging_calls(void)
{
    s_ranging_call_count = 0U;
    s_ranging_start_calls = 0U;
    s_ranging_progress_calls = 0U;
    s_ranging_invalidate_calls = 0U;
    s_ranging_cancel_calls = 0U;
    memset(s_ranging_calls, 0, sizeof(s_ranging_calls));
}

static unsigned *s_binary_semaphore;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1U, sizeof(fake_mutex_t));
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    s_binary_semaphore = calloc(1U, sizeof(*s_binary_semaphore));
    assert(s_binary_semaphore != NULL);
    return s_binary_semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    if (semaphore == s_binary_semaphore) {
        if (*s_binary_semaphore == 0U && !workflow_host_rtos_task_thread_running()) {
            workflow_host_rtos_run_created_task_sync();
        }
        if (*s_binary_semaphore == 0U) return pdFALSE;
        --*s_binary_semaphore;
        return pdTRUE;
    }
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL && !mutex->held);
    mutex->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == s_binary_semaphore) {
        *s_binary_semaphore = 1U;
        return pdTRUE;
    }
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL && mutex->held);
    mutex->held = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == s_binary_semaphore) s_binary_semaphore = NULL;
    free(semaphore);
}

int64_t esp_timer_get_time(void)
{
    return (int64_t)(s_now_ms * UINT64_C(1000));
}

void workflow_test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(s_last_log, sizeof(s_last_log), format, args);
    va_end(args);
    ++s_log_count;
}

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = s_measurement;
    return ESP_OK;
}

esp_err_t measurement_service_start_async(void) { return ESP_OK; }
esp_err_t measurement_service_stop_async(void) { return ESP_OK; }
esp_err_t measurement_service_delete_async(void) { return ESP_OK; }

esp_err_t object_service_get_active(object_service_summary_t *object)
{
    if (object == NULL) return ESP_ERR_INVALID_ARG;
    *object = s_object;
    return s_object.active ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = s_topology;
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_height_reference_capture(
    uint8_t *requested_channel_mask,
    uint16_t *requested_generation)
{
    if (requested_channel_mask == NULL || requested_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mask = 0U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (s_topology.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            mask |= (uint8_t)(1U << channel);
        }
    }
    if (mask == 0U) return ESP_ERR_NOT_FOUND;
    ++s_next_generation;
    if (s_next_generation == 0U) ++s_next_generation;
    s_requested_mask = mask;
    s_requested_generation = s_next_generation;
    *requested_channel_mask = mask;
    *requested_generation = s_requested_generation;
    ++s_height_requests;
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_imu_calibration(uint16_t generation, uint8_t channel_mask)
{
    assert(channel_mask == s_requested_mask);
    s_imu_generation = generation;
    ++s_imu_requests;
    return ESP_OK;
}

static nrf_topology_placement_heights_t s_placed_heights;
static unsigned s_placed_heights_sets;
static unsigned s_placed_heights_clears;

esp_err_t nrf_topology_service_set_placement_heights(const nrf_topology_placement_heights_t *heights)
{
    if (heights == NULL) {
        memset(&s_placed_heights, 0, sizeof(s_placed_heights));
        ++s_placed_heights_clears;
    } else {
        s_placed_heights = *heights;
        ++s_placed_heights_sets;
    }
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_sensor_snapshot(
    uint8_t channel,
    nrf_remote_sensor_cache_t *snapshot)
{
    if (channel >= NRF_TOPOLOGY_REMOTE_COUNT || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *snapshot = s_cache[channel];
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_cs_cycle(const nrf_cs_policy_t *policy)
{
    assert(policy != NULL);
    assert(policy->pair_timeout_ms == 2400U);     /* 1.5 x the longest attempt measured on the bench */
    assert(policy->max_attempts == 1U);
    assert(policy->min_confidence == 500U);
    assert(policy->samples_per_pair == 3U);
    ++s_cs_requests;
    ++s_cycle.cycle_id;
    s_cycle.active = true;
    s_cycle.complete = false;
    s_cycle.pair_count = 0U;
    s_cycle_valid = true;
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_cycle_valid) return ESP_ERR_INVALID_STATE;
    *snapshot = s_cycle;
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_start_autonomous_limited(const nrf_cs_policy_t *policy, const uint32_t *limits)
{
    assert(limits != NULL && limits[0] == 5000U && limits[3] == 5000U && limits[4] == 0U);
    return nrf_topology_service_ranging_start_autonomous(policy);
}

esp_err_t nrf_topology_service_ranging_start_autonomous(const nrf_cs_policy_t *policy)
{
    assert(policy != NULL);
    ranging_call(RANGING_CALL_START);
    ++s_ranging_start_calls;
    s_ranging_policy = *policy;
    /* Single-flight like the real service: a run still in flight rejects a new request. */
    if (s_ranging_start_result == ESP_OK && s_ranging_progress.active && !s_ranging_progress.complete) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ranging_start_result == ESP_OK) {
        memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
        s_ranging_progress.active = true;
        s_ranging_progress.started_at_ms = s_now_ms;
    }
    return s_ranging_start_result;
}

esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress)
{
    assert(progress != NULL);
    ranging_call(RANGING_CALL_PROGRESS);
    ++s_ranging_progress_calls;
    if (s_ranging_progress_result != ESP_OK) return s_ranging_progress_result;
    *progress = s_ranging_progress;
    return ESP_OK;
}

esp_err_t nrf_topology_service_invalidate_ranging(void)
{
    ranging_call(RANGING_CALL_INVALIDATE);
    ++s_ranging_invalidate_calls;
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_cancel(void)
{
    ++s_ranging_cancel_calls;
    if (!s_ranging_cancel_winds_down && s_ranging_progress.active && !s_ranging_progress.complete) {
        s_ranging_progress.active = false;
        s_ranging_progress.complete = true;
        s_ranging_progress.success = false;
        (void)snprintf(s_ranging_progress.failure_reason, sizeof(s_ranging_progress.failure_reason), "cancelled");
    }
    return ESP_OK;
}

/* The in-flight run finally stops (winding-down cancel); optionally with a result that must be ignored. */
static void release_fake_run(bool success)
{
    s_ranging_progress.active = false;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = success;
}

esp_err_t nrf_topology_service_ranging_set_manual(uint64_t started, uint64_t updated,
    uint8_t a, uint8_t b, int32_t distance, const uint32_t *limits)
{
    if (started != s_ranging_progress.started_at_ms || updated != s_ranging_progress.updated_at_ms)
        return ESP_ERR_INVALID_STATE;
    if (a != 0U || b != 2U || distance <= 0 || (uint32_t)distance > limits[1]) return ESP_ERR_INVALID_ARG;
    s_ranging_progress.pairs[1].automatic_distance_mm = s_ranging_progress.pairs[1].distance_mm;
    s_ranging_progress.pairs[1].distance_mm = distance;
    s_ranging_progress.pairs[1].state = NRF_CS_PAIR_VALID;
    s_ranging_progress.pairs[1].scheduler_quality_flags |= 1U << 6;
    s_ranging_progress.success = true;
    s_ranging_progress.partial = false;
    ++s_ranging_progress.updated_at_ms;
    s_ranging_progress.failure_reason[0] = '\0';
    return ESP_OK;
}

void main_barometer_request_polling(void)
{
}

esp_err_t main_barometer_request_height_reference_capture(uint16_t generation)
{
    s_main_generation = generation;
    ++s_main_requests;
    return generation == 0U ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t device_status_try_get_snapshot(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_device_result != ESP_OK) return s_device_result;
    *snapshot = s_device;
    return ESP_OK;
}

static void reset_fakes(void)
{
    workflow_service_shutdown();
    workflow_service_host_fakes_reset();
    memset(&s_measurement, 0, sizeof(s_measurement));
    s_measurement.state = MEASUREMENT_SERVICE_LIVE;
    memset(&s_object, 0, sizeof(s_object));
    s_object.active = true;
    (void)snprintf(s_object.id, sizeof(s_object.id), "%s", "object-a");
    memset(&s_topology, 0, sizeof(s_topology));
    s_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        s_topology.remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        s_topology.remotes[channel].node_id = (uint8_t)(channel + 1U);
    }
    memset(s_cache, 0, sizeof(s_cache));
    memset(&s_cycle, 0, sizeof(s_cycle));
    s_cycle_valid = false;
    memset(&s_device, 0, sizeof(s_device));
    s_device_result = ESP_OK;
    s_now_ms = 1000U;
    s_next_generation = 40U;
    s_requested_generation = 0U;
    s_imu_generation = 0U;
    s_main_generation = 0U;
    s_requested_mask = 0U;
    s_height_requests = 0U;
    s_imu_requests = 0U;
    s_cs_requests = 0U;
    s_main_requests = 0U;
    s_log_count = 0U;
    s_last_log[0] = '\0';
    memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
    memset(&s_ranging_policy, 0, sizeof(s_ranging_policy));
    s_ranging_start_result = ESP_OK;
    s_ranging_progress_result = ESP_OK;
    /* The real cancel is asynchronous: the run keeps going until the worker notices the flag. */
    s_ranging_cancel_winds_down = true;
    clear_ranging_calls();
    assert(workflow_service_init() == ESP_OK);
}

static workflow_command_result_t command(workflow_command_t value)
{
    const workflow_command_params_t params = {
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    return workflow_service_command(value, &params);
}

static void enter_calibrating(void)
{
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_height_requests == 1U);
    assert(s_imu_requests == 1U);
    /* Calibration is height + IMU only; Channel Sounding (BLE) starts at ranging. */
    assert(s_cs_requests == 0U);
    assert(s_main_requests == 1U);
    assert(s_requested_generation == s_imu_generation);
    assert(s_requested_generation == s_main_generation);
    assert(s_requested_mask == 0x07U);
    assert(s_log_count == 4U);
    assert(strstr(s_last_log, "WORKFLOW_CAL node=MAIN") == s_last_log);
}

static void publish_all_pass_results(void)
{
    const uint16_t generation = s_requested_generation;
    s_device.main.bmp280_valid = true;
    s_device.main.pressure_pa = 101325U;
    s_device.main.height_reference = (device_main_height_reference_status_t){
        .state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID,
        .generation = generation,
        .relative_z_mm = 0,
    };
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        s_cache[channel].identity_valid = true;
        s_cache[channel].topology_current = true;
        s_cache[channel].response_valid = true;
        s_cache[channel].identity.node_id = s_topology.remotes[channel].node_id;
        s_cache[channel].esp_receipt_uptime_ms = s_now_ms;
        s_cache[channel].sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
        s_cache[channel].sensor.pressure_pa = 101325U;
        s_cache[channel].sensor.altitude_mm = (int32_t)(12000U + 1000U * channel);
        s_cache[channel].sensor.orientation_state =
            NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
        s_cache[channel].sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
        s_cache[channel].sensor.reference_generation = generation;
        s_cache[channel].sensor.relative_z_mm = (int32_t)(100U * (channel + 1U));
        s_cache[channel].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
        s_cache[channel].sensor.imu_cal_generation = generation;
        s_cache[channel].sensor.gyro_bias_mdps[0] = (int16_t)(-10 - channel);
        s_cache[channel].sensor.gyro_bias_mdps[1] = (int16_t)(20 + channel);
        s_cache[channel].sensor.gyro_bias_mdps[2] = 3;
        s_cache[channel].sensor.gravity_ref_mg[0] = 10;
        s_cache[channel].sensor.gravity_ref_mg[1] = 20;
        s_cache[channel].sensor.gravity_ref_mg[2] = 990;
    }
}

static bool has_reason(const workflow_calibration_snapshot_t *calibration, const char *reason)
{
    for (size_t i = 0U; i < calibration->reasons_count; ++i) {
        if (strcmp(calibration->reasons[i], reason) == 0) return true;
    }
    return false;
}

static void test_calibration_all_pass(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.generation == s_requested_generation);
    assert(strcmp(snapshot.calibration.phase, "complete") == 0);
    assert(snapshot.calibration.result_valid && snapshot.calibration.result_ok);
    assert(snapshot.calibration.per_node_count == 4U);
    assert(strcmp(snapshot.calibration.per_node[0].node, "MAIN") == 0);
    assert(snapshot.calibration.per_node[0].relative_z_mm == 0);
    assert(strcmp(snapshot.calibration.per_node[2].node, "G2") == 0);
    assert(snapshot.calibration.per_node[2].gyro_bias_mdps[0] == -11);
    assert(snapshot.calibration.per_node[2].gravity_ref_mg[2] == 990);
    assert(snapshot.calibration.pairs_done == 0U && snapshot.calibration.pairs_total == 0U);
    assert(!snapshot.calibration.max_spread_valid);
    assert(snapshot.calibration.reasons_count == 0U);
    assert(s_log_count == 8U);
    workflow_service_shutdown();
}

static void assert_failure_reason(const char *expected)
{
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && !snapshot.calibration.result_ok);
    assert(has_reason(&snapshot.calibration, expected));
    workflow_service_shutdown();
}

static void test_calibration_height_rejected_moving_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cache[1].sensor.reference_state = NRF_UPSTREAM_REFERENCE_REJECTED_MOVING;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("G2:height_rejected_moving");
}

static void test_calibration_height_sensor_invalid_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("MAIN:height_sensor_invalid");
}

static void test_calibration_imu_rejected_moving_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cache[1].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("G2:imu_rejected_moving");
}

static void test_calibration_imu_rejected_gravity_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cache[1].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("G2:imu_rejected_gravity");
}

static void test_calibration_imu_sensor_invalid_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cache[1].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("G2:imu_sensor_invalid");
}

/* Whatever a stale or failed Channel Sounding cycle holds, calibration neither waits for it nor fails on it. */
static void test_calibration_ignores_channel_sounding_state(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cycle_valid = true;
    s_cycle.active = false;
    s_cycle.complete = true;
    s_cycle.pair_count = 3U;
    for (uint8_t i = 0U; i < 3U; ++i) {
        s_cycle.pairs[i] = (nrf_cs_pair_result_t){
            .node_a = NRF_TOPOLOGY_MAIN_NODE_ID,
            .node_b = (uint8_t)(i + 1U),
            .state = NRF_CS_PAIR_RETRY_EXHAUSTED,
            .spread_mm = 999,
        };
    }
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && snapshot.calibration.result_ok);
    assert(snapshot.calibration.reasons_count == 0U);
    assert(!has_reason(&snapshot.calibration, "cs_spread_too_high"));
    assert(s_cs_requests == 0U);
    workflow_service_shutdown();
}

static void test_calibration_timeout(void)
{
    reset_fakes();
    enter_calibrating();
    s_now_ms += 15000U;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("timeout");
}

static void test_calibration_back_cancels_pending_results(void)
{
    reset_fakes();
    enter_calibrating();
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_PREP);
    assert(strcmp(snapshot.calibration.phase, "cancelled") == 0);
    publish_all_pass_results();
    s_now_ms += 15000U;
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_PREP);
    assert(!snapshot.calibration.result_valid);
    assert(strcmp(snapshot.calibration.phase, "cancelled") == 0);
    workflow_service_shutdown();
}

static void test_calibration_retry_changes_generation_and_ignores_stale_results(void)
{
    reset_fakes();
    enter_calibrating();
    const uint16_t first_generation = s_requested_generation;
    publish_all_pass_results();
    s_cache[0].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING;
    assert(workflow_service_poll() == ESP_OK);
    assert(command(WORKFLOW_COMMAND_RETRY) == WORKFLOW_COMMAND_RESULT_OK);
    const uint16_t second_generation = s_requested_generation;
    assert(second_generation != first_generation);
    assert(s_imu_generation == second_generation && s_main_generation == second_generation);

    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIBRATING);
    assert(snapshot.calibration.generation == second_generation);
    assert(strcmp(snapshot.calibration.per_node[1].height_state, "pending") == 0);
    assert(strcmp(snapshot.calibration.per_node[1].imu_state, "pending") == 0);
    assert(snapshot.calibration.pairs_done == 0U);

    publish_all_pass_results();
    /* The helper now publishes generation two, not the late generation one. */
    assert(s_requested_generation == second_generation);
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_ok);
    workflow_service_shutdown();
}

static void enter_placement(void)
{
    enter_calibrating();
    publish_all_pass_results();
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && snapshot.calibration.result_ok);
    assert(command(WORKFLOW_COMMAND_PLACE_SENSORS) == WORKFLOW_COMMAND_RESULT_OK);
}

/* False while a test wants MAIN's barometer to deliver nothing (a read timed out, the sensor is being re-armed). */
static bool s_main_barometer_reads = true;

/* `remote_height_mm` is each node's own relative Z; MAIN's own relative Z (its barometer wandering) is `main_z_mm`. */
static void publish_placement_sample_with_main(
    uint64_t timestamp_ms,
    const int32_t remote_height_mm[3],
    const uint8_t orientation[3],
    int32_t main_z_mm)
{
    s_now_ms = timestamp_ms;
    s_device.main.bmp280_valid = s_main_barometer_reads;
    s_device.main.pressure_pa = 101325U;
    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device.main.height_reference.generation = s_requested_generation;
    s_device.main.height_reference.relative_z_mm = main_z_mm;
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        nrf_remote_sensor_cache_t *cache = &s_cache[channel];
        cache->identity_valid = true;
        cache->topology_current = true;
        cache->response_valid = true;
        cache->identity.node_id = s_topology.remotes[channel].node_id;
        cache->esp_receipt_uptime_ms = timestamp_ms;
        cache->sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
        cache->sensor.pressure_pa = 101325U;
        cache->sensor.altitude_mm = (int32_t)(12000U + 1000U * channel);
        cache->sensor.orientation_state = orientation[channel];
        cache->sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
        cache->sensor.reference_generation = s_requested_generation;
        cache->sensor.relative_z_mm = remote_height_mm[channel];
    }
    assert(workflow_service_poll() == ESP_OK);
}

static void publish_placement_sample(
    uint64_t timestamp_ms,
    const int32_t remote_height_mm[3],
    const uint8_t orientation[3])
{
    publish_placement_sample_with_main(timestamp_ms, remote_height_mm, orientation, 0);
}

static workflow_snapshot_t get_workflow_snapshot(void)
{
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

/* The REMOTE sensor refresh period of production (NRF_SENSOR_REFRESH_PERIOD_MS): one reading per node per 2 s. */
#define PLACEMENT_REFRESH_MS 2000U

/* Publishes `count` readings of the same heights, one refresh period apart, continuing from the current time. */
static void publish_steady_placement(
    const int32_t heights[3],
    const uint8_t orientation[3],
    size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        publish_placement_sample(s_now_ms + PLACEMENT_REFRESH_MS, heights, orientation);
    }
}

static void enter_ready_placement(void)
{
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(get_workflow_snapshot().placement.all_stable);
}

static void start_ranging(void)
{
    clear_ranging_calls();
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_GPS_CHOICE);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
}

static void test_ranging_start_is_invalidated_and_nonblocking(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    assert(s_ranging_start_calls == 1U);
    assert(s_ranging_progress_calls == 1U);
    assert(s_ranging_call_count == 3U);
    assert(s_ranging_calls[0] == RANGING_CALL_INVALIDATE);
    assert(s_ranging_calls[1] == RANGING_CALL_START);
    assert(s_ranging_calls[2] == RANGING_CALL_PROGRESS);
    assert(s_ranging_policy.full_rotation);
    assert(s_ranging_policy.pair_timeout_ms == 2400U);
    assert(s_ranging_policy.max_attempts == 1U);
    assert(s_ranging_policy.min_confidence == 500U);
    assert(s_ranging_policy.samples_per_pair == 3U);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING);
    assert(s_ranging_progress_calls == 2U);
    workflow_service_shutdown();
}

static void test_ranging_success_and_confirm_preserve_frozen_result(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);
    assert(get_workflow_snapshot().layout_result_valid);
    assert(get_workflow_snapshot().layout_ok);
    assert(s_ranging_invalidate_calls == 1U);
    assert(command(WORKFLOW_COMMAND_CONFIRM) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_SCAN_CHOICE);
    assert(command(WORKFLOW_COMMAND_SCAN_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RECORD_READY);
    assert(s_ranging_invalidate_calls == 1U);
    workflow_service_shutdown();
}

static void assert_ranging_failure_reason(const char *reason)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    s_ranging_progress.complete = true;
    s_ranging_progress.success = false;
    (void)snprintf(
        s_ranging_progress.failure_reason,
        sizeof(s_ranging_progress.failure_reason),
        "%s",
        reason);
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(snapshot.layout_result_valid && !snapshot.layout_ok);
    assert(strcmp(snapshot.reason, reason) == 0);
    assert(s_ranging_invalidate_calls == 2U);
    workflow_service_shutdown();
}

static void test_ranging_partial_pairs_are_saved(void)
{
    reset_fakes();
    workflow_test_saved_pairs_calls = 0U;
    workflow_test_saved_pairs_result = ESP_OK;
    enter_ready_placement();
    start_ranging();
    s_ranging_progress.complete = true;
    s_ranging_progress.partial = true;
    s_ranging_progress.pair_count = 2U;
    s_ranging_progress.pairs[0] = (nrf_cs_pair_result_t){.node_a = 0, .node_b = 1,
        .state = NRF_CS_PAIR_VALID, .distance_mm = 900};
    s_ranging_progress.pairs[1] = (nrf_cs_pair_result_t){.node_a = 0, .node_b = 2,
        .state = NRF_CS_PAIR_MISSING, .distance_mm = 0};
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);
    assert(workflow_test_saved_pairs_calls == 1U);
    assert(workflow_test_saved_pairs.pair_count == 2U);
    assert(workflow_test_saved_pairs.pairs[0].valid && workflow_test_saved_pairs.pairs[0].distance_mm == 900);
    assert(!workflow_test_saved_pairs.pairs[1].valid && workflow_test_saved_pairs.pairs[1].node_b == 2);
    assert(s_ranging_invalidate_calls == 1U);
    const workflow_snapshot_t partial = get_workflow_snapshot();
    workflow_command_params_t edit = {
        .has_generation = true, .expected_generation = partial.generation,
        .has_distance = true, .node_a = 0U, .node_b = 2U, .distance_mm = 5001,
        .ranging_started_at_ms = s_ranging_progress.started_at_ms,
        .ranging_updated_at_ms = s_ranging_progress.updated_at_ms,
    };
    assert(workflow_service_command(WORKFLOW_COMMAND_SET_DISTANCE, &edit) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    assert(workflow_test_saved_pairs_calls == 1U);
    edit.distance_mm = 1400;
    uint32_t seq = 0U;
    assert(workflow_service_set_distance_async(&edit, &seq) == ESP_OK);
    const workflow_snapshot_t completed = get_workflow_snapshot();
    assert(completed.last_async.seq == seq && completed.last_async.result == WORKFLOW_COMMAND_RESULT_OK);
    assert(completed.generation != partial.generation);
    assert(completed.reason[0] == '\0');
    assert((completed.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM)) != 0U);
    assert(workflow_test_saved_pairs_calls == 2U);
    assert(workflow_test_saved_pairs.pairs[1].valid && workflow_test_saved_pairs.pairs[1].distance_mm == 1400);
    assert((workflow_test_saved_pairs.pairs[1].scheduler_quality_flags & (1U << 6)) != 0U);
    assert(workflow_service_command(WORKFLOW_COMMAND_SET_DISTANCE, &edit) == WORKFLOW_COMMAND_RESULT_STALE);
    edit.expected_generation = completed.generation;
    edit.ranging_updated_at_ms = s_ranging_progress.updated_at_ms;
    workflow_test_saved_pairs_result = ESP_FAIL;
    assert(workflow_service_command(WORKFLOW_COMMAND_SET_DISTANCE, &edit) == WORKFLOW_COMMAND_RESULT_UNAVAILABLE);
    const workflow_snapshot_t failed_save = get_workflow_snapshot();
    assert((failed_save.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CONFIRM)) == 0U);
    assert(strcmp(failed_save.reason, "ranging_save_failed") == 0);
    workflow_test_saved_pairs_result = ESP_OK;
    workflow_service_shutdown();
}

static void test_ranging_failure_reasons_propagate(void)
{
    assert_ranging_failure_reason("pair_failed:1-2");
    assert_ranging_failure_reason("geometry_unsolved:3");
    assert_ranging_failure_reason("timeout");
}

static void test_ranging_start_error_reports_failure(void)
{
    reset_fakes();
    enter_ready_placement();
    s_ranging_start_result = ESP_ERR_INVALID_STATE;
    start_ranging();
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(strcmp(snapshot.reason, "start_failed:ESP_ERR_INVALID_STATE") == 0);
    assert(s_ranging_start_calls == 1U);
    assert(s_ranging_progress_calls == 0U);
    assert(s_ranging_call_count == 3U);
    assert(s_ranging_calls[0] == RANGING_CALL_INVALIDATE);
    assert(s_ranging_calls[1] == RANGING_CALL_START);
    assert(s_ranging_calls[2] == RANGING_CALL_INVALIDATE);
    workflow_service_shutdown();
}

static void test_ranging_retry_starts_a_new_run_and_ignores_stale_completion(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    const uint64_t previous_started_at_ms = s_ranging_progress.started_at_ms;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = false;
    (void)snprintf(
        s_ranging_progress.failure_reason,
        sizeof(s_ranging_progress.failure_reason),
        "%s",
        "timeout");
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);

    s_now_ms += 100U;
    clear_ranging_calls();
    assert(command(WORKFLOW_COMMAND_RETRY) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_snapshot_t retry_snapshot = get_workflow_snapshot();
    assert(retry_snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(!retry_snapshot.layout_result_valid);
    assert(s_ranging_start_calls == 0U);
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_ranging_start_calls == 1U);
    assert(s_ranging_progress_calls == 1U);
    assert(s_ranging_call_count == 3U);
    assert(s_ranging_calls[0] == RANGING_CALL_INVALIDATE);
    assert(s_ranging_calls[1] == RANGING_CALL_START);
    assert(s_ranging_calls[2] == RANGING_CALL_PROGRESS);
    const uint64_t current_started_at_ms = s_ranging_progress.started_at_ms;
    assert(current_started_at_ms != previous_started_at_ms);

    s_ranging_progress.started_at_ms = previous_started_at_ms;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING);

    s_ranging_progress.started_at_ms = current_started_at_ms;
    s_ranging_progress.complete = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);
    workflow_service_shutdown();
}

static void test_ranging_back_orphans_run_and_invalidates_late_result(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    const uint64_t started_at_ms = s_ranging_progress.started_at_ms;
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_invalidate_calls == 2U);

    s_ranging_progress.started_at_ms = started_at_ms;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_invalidate_calls == 3U);
    workflow_service_shutdown();
}

/* Back from LAYOUT_READY into RANGING; placement history must be rebuilt after BACK. */
static void reenter_ranging_after_back(void)
{
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
}

static bool ranging_failed_to_start(const workflow_snapshot_t *snapshot)
{
    return strstr(snapshot->reason, "start_failed") != NULL;
}

static void test_back_from_ranging_cancels_active_run(void)
{
    reset_fakes();
    s_ranging_cancel_winds_down = false;
    enter_ready_placement();
    start_ranging();
    assert(s_ranging_cancel_calls == 0U);
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_cancel_calls == 1U);
    assert(!s_ranging_progress.active);
    workflow_service_shutdown();
}

static void test_back_then_reenter_starts_new_run(void)
{
    reset_fakes();
    s_ranging_cancel_winds_down = false;
    enter_ready_placement();
    start_ranging();
    const uint64_t old_started_at_ms = s_ranging_progress.started_at_ms;
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);

    reenter_ranging_after_back();
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING);
    assert(!ranging_failed_to_start(&snapshot));
    assert(s_ranging_start_calls == 2U);
    const uint64_t new_started_at_ms = s_ranging_progress.started_at_ms;
    assert(new_started_at_ms != old_started_at_ms);

    /* A late completion of the cancelled run is never applied to the new one. */
    s_ranging_progress.started_at_ms = old_started_at_ms;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING);

    s_ranging_progress.started_at_ms = new_started_at_ms;
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(snapshot.layout_result_valid && snapshot.layout_ok);
    workflow_service_shutdown();
}

/* BACK, then straight back into RANGING while the old cycle is still winding down. */
static uint64_t enter_ranging_while_old_run_winds_down(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    const uint64_t old_started_at_ms = s_ranging_progress.started_at_ms;
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_ranging_cancel_calls == 1U);
    assert(s_ranging_progress.active && !s_ranging_progress.complete);

    reenter_ranging_after_back();
    assert(s_ranging_start_calls == 2U);
    return old_started_at_ms;
}

static void test_reenter_while_old_run_winding_down_defers_start(void)
{
    const uint64_t old_started_at_ms = enter_ranging_while_old_run_winds_down();
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING);
    assert(!ranging_failed_to_start(&snapshot));

    /* Old run still in flight: nothing is retried and nothing fails. */
    assert(workflow_service_poll() == ESP_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING);
    assert(s_ranging_start_calls == 2U);

    /* Old run stops, even with a "success" that must never be applied to the new request. */
    s_now_ms += 100U;
    release_fake_run(true);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING);
    assert(!ranging_failed_to_start(&snapshot));
    assert(s_ranging_start_calls == 3U);
    assert(s_ranging_progress.active && !s_ranging_progress.complete);
    const uint64_t new_started_at_ms = s_ranging_progress.started_at_ms;
    assert(new_started_at_ms != old_started_at_ms);

    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(snapshot.layout_result_valid && snapshot.layout_ok);
    workflow_service_shutdown();
}

static void test_leave_ranging_again_cancels_deferred_start(void)
{
    (void)enter_ranging_while_old_run_winds_down();
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_cancel_calls == 2U);

    s_now_ms += 100U;
    release_fake_run(true);
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_start_calls == 2U);
    workflow_service_shutdown();
}

static void test_deferred_start_gives_up_when_old_run_never_stops(void)
{
    (void)enter_ranging_while_old_run_winds_down();
    s_now_ms += 60000U;
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    assert(strcmp(snapshot.reason, "start_failed:ESP_ERR_INVALID_STATE") == 0);
    assert(s_ranging_start_calls == 2U);

    /* The straggler finishing later changes nothing. */
    release_fake_run(true);
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);
    assert(s_ranging_start_calls == 2U);
    workflow_service_shutdown();
}

static void test_start_errors_without_orphaned_run_fail_immediately(void)
{
    reset_fakes();
    enter_ready_placement();
    s_ranging_start_result = ESP_ERR_NO_MEM;
    start_ranging();
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
    char expected[WORKFLOW_REASON_MAX];
    (void)snprintf(expected, sizeof(expected), "start_failed:%s", esp_err_to_name(ESP_ERR_NO_MEM));
    assert(strcmp(snapshot.reason, expected) == 0);
    assert(s_ranging_start_calls == 1U);
    workflow_service_shutdown();
}

static void test_ranging_back_and_retry_race(void)
{
    test_back_from_ranging_cancels_active_run();
    test_back_then_reenter_starts_new_run();
    test_reenter_while_old_run_winding_down_defers_start();
    test_leave_ranging_again_cancels_deferred_start();
    test_deferred_start_gives_up_when_old_run_never_stops();
    test_start_errors_without_orphaned_run_fail_immediately();
}

static void test_max460_layout_success_failure_reason_retry_and_back(void)
{
    test_ranging_success_and_confirm_preserve_frozen_result();
    test_ranging_failure_reasons_propagate();
    test_ranging_partial_pairs_are_saved();
    test_ranging_start_error_reports_failure();
    test_ranging_retry_starts_a_new_run_and_ignores_stale_completion();
    test_ranging_back_orphans_run_and_invalidates_late_result();
    test_ranging_back_and_retry_race();
}

static void test_placement_entry_invalidates_back_and_move_sensors(void)
{
    reset_fakes();
    enter_ready_placement();
    start_ranging();
    s_ranging_progress.complete = true;
    s_ranging_progress.success = false;
    (void)snprintf(
        s_ranging_progress.failure_reason,
        sizeof(s_ranging_progress.failure_reason),
        "%s",
        "timeout");
    assert(workflow_service_poll() == ESP_OK);
    clear_ranging_calls();
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_LAYOUT_READY);
    assert(s_ranging_invalidate_calls == 0U);
    assert(s_ranging_call_count == 0U);
    workflow_service_shutdown();

    reset_fakes();
    enter_ready_placement();
    start_ranging();
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(command(WORKFLOW_COMMAND_CONFIRM) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SCAN_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    clear_ranging_calls();
    assert(command(WORKFLOW_COMMAND_MOVE_SENSORS) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_PLACEMENT);
    assert(s_ranging_invalidate_calls == 1U);
    assert(s_ranging_call_count == 1U);
    assert(s_ranging_calls[0] == RANGING_CALL_INVALIDATE);
    workflow_service_shutdown();
}

/* One reading per node per refresh period; series[i] holds the three REMOTE heights of reading i. */
static void publish_series(const int32_t series[][3], size_t count)
{
    const uint8_t upright[3] = { 2U, 2U, 2U };
    for (size_t i = 0U; i < count; ++i) {
        publish_placement_sample(s_now_ms + PLACEMENT_REFRESH_MS, series[i], upright);
    }
}

static void test_placement_needs_a_full_window_of_samples_and_reports_their_mean(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(!snapshot.placement.per_node[0].stable);
    assert(snapshot.placement.per_node[1].sample_count == WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_COLLECTING);
    assert(snapshot.placement.per_node[1].range_mm == -1);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);

    publish_steady_placement(heights, upright, 1U);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
    assert(snapshot.placement.per_node_count == 4U);
    assert(snapshot.placement.per_node[0].vertical);
    assert(snapshot.placement.per_node[0].stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 100);
    assert(snapshot.placement.per_node[2].height_rel_main_mm == 200);
    assert(snapshot.placement.per_node[3].height_rel_main_mm == 300);
    assert(snapshot.placement.per_node[1].range_mm == 0);
    assert(snapshot.placement.per_node[1].altitude_asl_m == 12.0);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_placement_reports_the_window_mean_not_the_last_reading(void)
{
    reset_fakes();
    enter_placement();
    int32_t series[8][3];
    for (size_t i = 0U; i < 8U; ++i) {
        series[i][0] = (i % 2U) == 0U ? 40 : 160;   /* the last reading is 160, the mean 100 */
        series[i][1] = 200;
        series[i][2] = 300;
    }
    publish_series(series, 8U);
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 100);
}

/* Node 1 alternates around 200 by +-amplitude; the mean is 200 and the 95 % half-width is 0.7559 * amplitude. */
static bool alternating_node_is_stable(int32_t amplitude, int32_t *range_mm, workflow_placement_reason_t *reason)
{
    reset_fakes();
    enter_placement();
    int32_t series[8][3];
    for (size_t i = 0U; i < 8U; ++i) {
        series[i][0] = 200 + ((i % 2U) == 0U ? amplitude : -amplitude);
        series[i][1] = 200;
        series[i][2] = 300;
    }
    publish_series(series, 8U);
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    *range_mm = snapshot.placement.per_node[1].range_mm;
    *reason = snapshot.placement.per_node[1].reason;
    const bool stable = snapshot.placement.per_node[1].stable;
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 200);
    /* The verdict is what gates the next step, not only a flag on the screen. */
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) ==
        (stable ? WORKFLOW_COMMAND_RESULT_OK : WORKFLOW_COMMAND_RESULT_NOT_ALLOWED));
    workflow_service_shutdown();
    return stable;
}

static void test_placement_uncertainty_of_the_mean_is_bounded_at_100_mm(void)
{
    int32_t range_mm = 0;
    workflow_placement_reason_t reason = WORKFLOW_PLACEMENT_REASON_COLLECTING;
    assert(alternating_node_is_stable(132, &range_mm, &reason));
    assert(range_mm == WORKFLOW_PLACEMENT_MAX_RANGE_MM);
    assert(reason == WORKFLOW_PLACEMENT_REASON_STABLE);

    assert(!alternating_node_is_stable(133, &range_mm, &reason));
    assert(range_mm == WORKFLOW_PLACEMENT_MAX_RANGE_MM + 1);
    assert(reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
}

/* Node 1 reads `base` ten times, with `stray` at the given positions (a spurious reading each). */
static void publish_series_with_strays(int32_t base, int32_t stray, const size_t *positions, size_t position_count)
{
    int32_t series[10][3];
    for (size_t i = 0U; i < 10U; ++i) {
        series[i][0] = base;
        for (size_t k = 0U; k < position_count; ++k) {
            if (positions[k] == i) series[i][0] = stray;
        }
        series[i][1] = 200;
        series[i][2] = 300;
    }
    publish_series(series, 10U);
}

static void test_placement_drops_one_stray_reading_but_not_two(void)
{
    /* One reading 900 mm off in ten is an outlier (a bus glitch, a puff of air): the height stays at 200 mm. */
    const size_t one[] = { 5U };
    reset_fakes();
    enter_placement();
    publish_series_with_strays(200, 1100, one, 1U);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 200);
    assert(snapshot.placement.per_node[1].range_mm == 0);
    workflow_service_shutdown();

    /* Two of ten is not a glitch: the spread of every reading decides and the node is not accepted. */
    const size_t two[] = { 5U, 9U };
    reset_fakes();
    enter_placement();
    publish_series_with_strays(200, 1100, two, 2U);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(snapshot.placement.per_node[1].range_mm > WORKFLOW_PLACEMENT_MAX_RANGE_MM);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    workflow_service_shutdown();
}

static void test_placement_refuses_a_node_that_was_carried_to_another_height(void)
{
    /* Seven readings at 100 mm, then four at 600 mm: the operator is still moving the node. */
    reset_fakes();
    enter_placement();
    int32_t series[11][3];
    for (size_t i = 0U; i < 11U; ++i) {
        series[i][0] = i < 7U ? 100 : 600;
        series[i][1] = 200;
        series[i][2] = 300;
    }
    publish_series(series, 11U);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].sample_count == 11U);
    assert(!snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(snapshot.placement.per_node[1].range_mm > WORKFLOW_PLACEMENT_MAX_RANGE_MM);
    assert(!snapshot.placement.all_stable);
    workflow_service_shutdown();
}

static void test_placement_cancels_the_wander_of_the_main_barometer(void)
{
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const int32_t main_z[8] = { -300, 250, -100, 400, -250, 150, 0, -350 };

    /* Every node follows MAIN's wander (same air): the difference is constant. */
    reset_fakes();
    enter_placement();
    for (size_t i = 0U; i < 8U; ++i) {
        const int32_t heights[3] = { 500 + main_z[i], 700 + main_z[i], 900 + main_z[i] };
        publish_placement_sample_with_main(s_now_ms + PLACEMENT_REFRESH_MS, heights, upright, main_z[i]);
    }
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 500);
    assert(snapshot.placement.per_node[2].height_rel_main_mm == 700);
    assert(snapshot.placement.per_node[3].height_rel_main_mm == 900);
    assert(snapshot.placement.per_node[1].range_mm == 0);
    workflow_service_shutdown();

    /* The nodes do not follow it (the bench: a slow filter on the node, a fast one on MAIN): it is noise. */
    reset_fakes();
    enter_placement();
    const int32_t still[3] = { 500, 700, 900 };
    for (size_t i = 0U; i < 8U; ++i) {
        publish_placement_sample_with_main(s_now_ms + PLACEMENT_REFRESH_MS, still, upright, main_z[i]);
    }
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(snapshot.placement.per_node[1].range_mm > WORKFLOW_PLACEMENT_MAX_RANGE_MM);
    workflow_service_shutdown();
}

/*
 * The heights G1, G2 and G4 reported relative to MAIN in esp32_COM3_2026-11-01_18-16-16.log, one row per poll
 * with all three present (milliseconds of the log, then millimetres). The old rule (spread of the last
 * readings <= 50 mm) never accepted them: the spread of these rows is 221, 276 and 384 mm. Expected values were
 * computed independently (two-pass mean and sample deviation) and are rounded as the firmware rounds them.
 */
static void test_placement_accepts_the_noisy_bench_log_of_2026_11_01(void)
{
    static const struct { uint64_t log_ms; int32_t height[3]; } rows[] = {
        { 31500U, { -168, -168, -98 } },
        { 33600U, { -241, -185, -170 } },
        { 35800U, { -204, -197, -79 } },
        { 38000U, { -90, -32, 39 } },
        { 40200U, { -154, -78, 0 } },
        { 42400U, { -20, 61, 191 } },
        { 44400U, { -63, 20, 167 } },
        { 46400U, { -113, 27, 123 } },
        { 48600U, { -23, 79, 214 } },
        { 50600U, { -212, -122, 27 } },
    };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    reset_fakes();
    enter_placement();
    const uint64_t origin_ms = s_now_ms + 1000U;
    for (size_t i = 0U; i < 10U; ++i) {
        publish_placement_sample(origin_ms + (rows[i].log_ms - rows[0].log_ms), rows[i].height, upright);
        const workflow_snapshot_t snapshot = get_workflow_snapshot();
        if (i + 1U < WORKFLOW_PLACEMENT_MIN_SAMPLES) {
            assert(!snapshot.placement.all_stable);
            assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_COLLECTING);
            continue;
        }
        assert(snapshot.placement.all_stable);
        if (i == 7U) {
            assert(snapshot.placement.per_node[1].height_rel_main_mm == -132);
            assert(snapshot.placement.per_node[2].height_rel_main_mm == -69);
            assert(snapshot.placement.per_node[3].height_rel_main_mm == 22);
            assert(snapshot.placement.per_node[1].range_mm == 53);
            assert(snapshot.placement.per_node[2].range_mm == 74);
            assert(snapshot.placement.per_node[3].range_mm == 94);
        }
    }
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].height_rel_main_mm == -129);
    assert(snapshot.placement.per_node[2].height_rel_main_mm == -60);
    assert(snapshot.placement.per_node[3].height_rel_main_mm == 41);
    assert(snapshot.placement.per_node[1].range_mm == 51);
    assert(snapshot.placement.per_node[2].range_mm == 67);
    assert(snapshot.placement.per_node[3].range_mm == 84);
    /* The geometry of the ranging and of the recording is given these heights, not the reading of the instant. */
    assert(s_placed_heights.valid && s_placed_heights.generation == s_requested_generation);
    assert(s_placed_heights.node_valid[0] && s_placed_heights.height_rel_main_mm[0] == -129);
    assert(s_placed_heights.node_valid[1] && s_placed_heights.height_rel_main_mm[1] == -60);
    assert(s_placed_heights.node_valid[2] && s_placed_heights.height_rel_main_mm[2] == 41);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_settled_heights_are_given_to_the_geometry_and_dropped_when_going_back(void)
{
    reset_fakes();
    memset(&s_placed_heights, 0, sizeof(s_placed_heights));
    enter_placement();
    s_placed_heights_clears = 0U;   /* the steps before the placement clear the latch on every poll */
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };

    /* While a node is still collecting nothing is given for it: the geometry must not take a half-settled value. */
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U);
    assert(!s_placed_heights.node_valid[0] && !s_placed_heights.node_valid[1] && !s_placed_heights.node_valid[2]);

    publish_steady_placement(heights, upright, 1U);
    assert(s_placed_heights.valid);
    assert(s_placed_heights.generation == s_requested_generation);
    for (size_t channel = 0U; channel < 3U; ++channel) {
        assert(s_placed_heights.node_valid[channel]);
        assert(s_placed_heights.node_id[channel] == s_topology.remotes[channel].node_id);
        assert(s_placed_heights.height_rel_main_mm[channel] == heights[channel]);
    }

    /* A node that stops being vertical is withdrawn while the others stay. */
    const uint8_t tilted[3] = { 2U, 1U, 2U };
    publish_placement_sample(s_now_ms + PLACEMENT_REFRESH_MS, heights, tilted);
    assert(s_placed_heights.node_valid[0] && !s_placed_heights.node_valid[1] && s_placed_heights.node_valid[2]);
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(s_placed_heights.node_valid[1]);

    /* The ranging and the recording keep what the placement settled: the nodes are not touched meanwhile. */
    start_ranging();
    s_now_ms += PLACEMENT_REFRESH_MS;
    assert(workflow_service_poll() == ESP_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_RANGING);
    assert(s_placed_heights.valid && s_placed_heights.node_valid[1]);
    assert(s_placed_heights_clears == 0U);
    workflow_service_shutdown();

    /* Going back to before the placement drops them: the nodes will be moved. */
    reset_fakes();
    s_placed_heights_clears = 0U;
    enter_ready_placement();
    assert(s_placed_heights.valid);
    assert(command(WORKFLOW_COMMAND_BACK) == WORKFLOW_COMMAND_RESULT_OK);
    assert(get_workflow_snapshot().step == WORKFLOW_STEP_CALIB_RESULT);
    s_now_ms += PLACEMENT_REFRESH_MS;
    assert(workflow_service_poll() == ESP_OK);
    assert(s_placed_heights_clears >= 1U);
    assert(!s_placed_heights.valid);
    workflow_service_shutdown();
}

/*
 * Bench log 2026-11-01 21:46: MAIN's barometer missed a reading every ~6 s for half a minute (a read timed out
 * while core 0 was busy, then a 5 s pause before the re-arm). Each miss wiped all collected samples, so the
 * stability that needs 16 s of readings took 76 s.
 */
static void test_placement_keeps_its_samples_through_a_short_main_barometer_outage(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };

    publish_steady_placement(heights, upright, 5U);
    assert(get_workflow_snapshot().placement.per_node[1].sample_count == 5U);

    s_main_barometer_reads = false;
    publish_steady_placement(heights, upright, 3U);                 /* six seconds without a barometer */
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].sample_count == 5U);      /* kept, none added */
    assert(snapshot.placement.per_node[0].sample_count == 5U);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_COLLECTING);
    assert(!snapshot.placement.all_stable);

    s_main_barometer_reads = true;
    publish_steady_placement(heights, upright, 2U);
    assert(get_workflow_snapshot().placement.per_node[1].sample_count == 7U);
    assert(!get_workflow_snapshot().placement.all_stable);
    publish_steady_placement(heights, upright, 1U);                 /* the eighth reading with a barometer */
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 100);
    workflow_service_shutdown();
}

static void test_placement_that_is_stable_stays_stable_through_a_short_main_barometer_outage(void)
{
    reset_fakes();
    enter_ready_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    s_main_barometer_reads = false;
    for (unsigned i = 0U; i < WORKFLOW_PLACEMENT_MAIN_BARO_GRACE_MS / PLACEMENT_REFRESH_MS; ++i) {
        publish_steady_placement(heights, upright, 1U);             /* up to 14 s blind */
        const workflow_snapshot_t snapshot = get_workflow_snapshot();
        assert(snapshot.placement.all_stable);
        assert(snapshot.placement.per_node[1].height_rel_main_mm == 100);
    }
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    s_main_barometer_reads = true;
    workflow_service_shutdown();
}

static void test_placement_gives_up_when_the_main_barometer_stays_blind_too_long(void)
{
    reset_fakes();
    enter_ready_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    s_main_barometer_reads = false;
    const unsigned held = WORKFLOW_PLACEMENT_MAIN_BARO_GRACE_MS / PLACEMENT_REFRESH_MS;   /* 7 readings, 14 s */
    publish_steady_placement(heights, upright, held);
    assert(get_workflow_snapshot().placement.all_stable);
    publish_steady_placement(heights, upright, 1U);                 /* 16 s: past the grace */
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER);
    assert(snapshot.placement.per_node[1].sample_count == 0U);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    s_main_barometer_reads = true;
    workflow_service_shutdown();
}

static void test_placement_does_not_count_readings_closer_than_the_gap(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const uint64_t base_ms = s_now_ms;
    /* 30 readings 100 ms apart: three seconds of air, not thirty independent samples. */
    for (uint64_t i = 1U; i <= 30U; ++i) {
        publish_placement_sample(base_ms + 100U * i, heights, upright);
    }
    const workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].sample_count == 3U);
    assert(!snapshot.placement.per_node[1].stable);
    assert(!snapshot.placement.per_node[0].stable);
    assert(!snapshot.placement.all_stable);
    workflow_service_shutdown();
}

static void test_placement_window_boundary_is_inclusive(void)
{
    reset_fakes();
    enter_placement();
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const int32_t outlier[3] = { 1000, 200, 300 };
    const int32_t steady[3] = { 0, 200, 300 };
    const uint64_t outlier_ms = s_now_ms + PLACEMENT_REFRESH_MS;
    /* Two stray readings (one alone would be dropped as an outlier); the first leaves the window at the boundary. */
    publish_placement_sample(outlier_ms, outlier, upright);
    publish_placement_sample(outlier_ms + PLACEMENT_REFRESH_MS, outlier, upright);
    for (uint64_t k = 2U; k <= WORKFLOW_PLACEMENT_WINDOW_MS / PLACEMENT_REFRESH_MS; ++k) {
        publish_placement_sample(outlier_ms + k * PLACEMENT_REFRESH_MS, steady, upright);
    }
    /* A reading exactly one window old is still inside it... */
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].sample_count == 11U);
    assert(!snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    /* ...and one millisecond later it has left (this reading is within the gap of the last, so it is not kept). */
    publish_placement_sample(outlier_ms + WORKFLOW_PLACEMENT_WINDOW_MS + 1U, steady, upright);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].sample_count == 10U);
    assert(snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 0);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_layout_rechecks_placement_after_gps_choice(void)
{
    reset_fakes();
    enter_ready_placement();
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);

    /* A sensor is carried 600 mm higher while the operator decides about GPS. */
    const int32_t moved[3] = { 100, 800, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(moved, upright, 4U);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_CHOICE);
    assert(!snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[2].reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);

    assert(command(WORKFLOW_COMMAND_GPS_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(!snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);

    /* Once the old readings have left the window the node is stable again, at its new height. */
    publish_steady_placement(moved, upright, 12U);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
    assert(snapshot.placement.per_node[2].height_rel_main_mm == 800);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_placement_vertical_flip_resets_sample_window(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const uint8_t tilted[3] = { 2U, 1U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[2].stable);

    publish_placement_sample(s_now_ms + PLACEMENT_REFRESH_MS, heights, tilted);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].vertical);
    assert(!snapshot.placement.per_node[2].stable);
    assert(!snapshot.placement.all_stable);

    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].stable);
    assert(snapshot.placement.per_node[2].sample_count == WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U);
    publish_steady_placement(heights, upright, 1U);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[2].stable);
    workflow_service_shutdown();
}

static void test_placement_generation_mismatch_and_node_dropout_are_unknown(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_steady_placement(heights, upright, WORKFLOW_PLACEMENT_MIN_SAMPLES);
    s_cache[1].sensor.reference_generation++;
    s_now_ms += PLACEMENT_REFRESH_MS;
    s_cache[1].esp_receipt_uptime_ms = s_now_ms;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].height_rel_main_mm_valid);
    assert(!snapshot.placement.per_node[2].stable);
    assert(!snapshot.placement.all_stable);

    /* The reference matches again: the node starts a new window instead of reusing the old readings. */
    s_cache[1].sensor.reference_generation = s_requested_generation;
    publish_steady_placement(heights, upright, 1U);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[2].sample_count == 1U);
    assert(!snapshot.placement.per_node[2].stable);
    s_cache[2].response_valid = false;
    s_now_ms += PLACEMENT_REFRESH_MS;
    s_cache[2].esp_receipt_uptime_ms = s_now_ms;
    assert(workflow_service_poll() == ESP_OK);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[3].vertical);
    assert(!snapshot.placement.per_node[3].height_rel_main_mm_valid);
    assert(!snapshot.placement.per_node[3].altitude_asl_m_valid);
    assert(!snapshot.placement.per_node[3].stable);
    assert(!snapshot.placement.all_stable);
    workflow_service_shutdown();
}

static void test_main_is_vertical_but_needs_a_valid_height_reference(void)
{
    reset_fakes();
    enter_placement();
    s_device.main.height_reference.state =
        DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED;
    s_now_ms = 1100U;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[0].vertical);
    assert(!snapshot.placement.per_node[0].height_rel_main_mm_valid);
    assert(!snapshot.placement.per_node[0].stable);
    assert(!snapshot.placement.all_stable);
    workflow_service_shutdown();
}

int main(void)
{
    test_calibration_all_pass();
    test_calibration_height_rejected_moving_reason();
    test_calibration_height_sensor_invalid_reason();
    test_calibration_imu_rejected_moving_reason();
    test_calibration_imu_rejected_gravity_reason();
    test_calibration_imu_sensor_invalid_reason();
    test_calibration_ignores_channel_sounding_state();
    test_calibration_timeout();
    test_calibration_back_cancels_pending_results();
    test_calibration_retry_changes_generation_and_ignores_stale_results();
    test_ranging_start_is_invalidated_and_nonblocking();
    test_max460_layout_success_failure_reason_retry_and_back();
    test_placement_entry_invalidates_back_and_move_sensors();
    test_placement_needs_a_full_window_of_samples_and_reports_their_mean();
    test_placement_reports_the_window_mean_not_the_last_reading();
    test_placement_uncertainty_of_the_mean_is_bounded_at_100_mm();
    test_placement_drops_one_stray_reading_but_not_two();
    test_placement_refuses_a_node_that_was_carried_to_another_height();
    test_placement_cancels_the_wander_of_the_main_barometer();
    test_placement_accepts_the_noisy_bench_log_of_2026_11_01();
    test_settled_heights_are_given_to_the_geometry_and_dropped_when_going_back();
    test_placement_keeps_its_samples_through_a_short_main_barometer_outage();
    test_placement_that_is_stable_stays_stable_through_a_short_main_barometer_outage();
    test_placement_gives_up_when_the_main_barometer_stays_blind_too_long();
    test_placement_does_not_count_readings_closer_than_the_gap();
    test_placement_window_boundary_is_inclusive();
    test_layout_rechecks_placement_after_gps_choice();
    test_placement_vertical_flip_resets_sample_window();
    test_placement_generation_mismatch_and_node_dropout_are_unknown();
    test_main_is_vertical_but_needs_a_valid_height_reference();
    return 0;
}
