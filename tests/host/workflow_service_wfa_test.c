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
static uint32_t s_ranging_timeout_ms;
static unsigned s_ranging_start_calls;
static unsigned s_ranging_progress_calls;
static unsigned s_ranging_invalidate_calls;
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
    assert(policy->pair_timeout_ms == 500U);
    assert(policy->max_attempts == 3U);
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

esp_err_t nrf_topology_service_ranging_start(const nrf_cs_policy_t *policy, uint32_t timeout_ms)
{
    assert(policy != NULL);
    ranging_call(RANGING_CALL_START);
    ++s_ranging_start_calls;
    s_ranging_policy = *policy;
    s_ranging_timeout_ms = timeout_ms;
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
    s_ranging_timeout_ms = 0U;
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
    assert(s_cs_requests == 1U);
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
    s_cycle.active = false;
    s_cycle.complete = true;
    s_cycle.pair_count = 3U;
    for (uint8_t i = 0U; i < 3U; ++i) {
        s_cycle.pairs[i] = (nrf_cs_pair_result_t){
            .node_a = NRF_TOPOLOGY_MAIN_NODE_ID,
            .node_b = (uint8_t)(i + 1U),
            .state = NRF_CS_PAIR_VALID,
            .spread_mm = (int32_t)(i == 2U ? 150U : 10U + i),
        };
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
    assert(snapshot.calibration.per_node[0].cs_pairs_ok);
    assert(strcmp(snapshot.calibration.per_node[2].node, "G2") == 0);
    assert(snapshot.calibration.per_node[2].gyro_bias_mdps[0] == -11);
    assert(snapshot.calibration.per_node[2].gravity_ref_mg[2] == 990);
    assert(snapshot.calibration.pairs_done == 3U && snapshot.calibration.pairs_total == 3U);
    assert(snapshot.calibration.max_spread_valid && snapshot.calibration.max_spread_mm == 150);
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

static void test_calibration_cs_pair_invalid_reason_and_node_isolation(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cycle.pairs[1].state = NRF_CS_PAIR_RETRY_EXHAUSTED;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(has_reason(&snapshot.calibration, "cs_pair_invalid:0-2"));
    assert(!snapshot.calibration.per_node[2].cs_pairs_ok);
    assert(snapshot.calibration.per_node[1].cs_pairs_ok);
    assert(snapshot.calibration.per_node[3].cs_pairs_ok);
    assert(snapshot.calibration.pairs_done == 3U);
    workflow_service_shutdown();
}

static void test_calibration_cs_spread_reason(void)
{
    reset_fakes();
    enter_calibrating();
    publish_all_pass_results();
    s_cycle.pairs[2].spread_mm = 151;
    assert(workflow_service_poll() == ESP_OK);
    assert_failure_reason("cs_spread_too_high");
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

static void publish_placement_sample(
    uint64_t timestamp_ms,
    const int32_t remote_height_mm[3],
    const uint8_t orientation[3])
{
    s_now_ms = timestamp_ms;
    s_device.main.bmp280_valid = true;
    s_device.main.pressure_pa = 101325U;
    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device.main.height_reference.generation = s_requested_generation;
    s_device.main.height_reference.relative_z_mm = 0;
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

static workflow_snapshot_t get_workflow_snapshot(void)
{
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

static void enter_ready_placement(void)
{
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_placement_sample(1000U, heights, upright);
    publish_placement_sample(1100U, heights, upright);
    publish_placement_sample(1200U, heights, upright);
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
    assert(s_ranging_policy.pair_timeout_ms == 500U);
    assert(s_ranging_policy.max_attempts == 3U);
    assert(s_ranging_policy.min_confidence == 500U);
    assert(s_ranging_policy.samples_per_pair == 3U);
    assert(s_ranging_timeout_ms == NRF_TOPOLOGY_RANGING_DEFAULT_TIMEOUT_MS);
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
    publish_placement_sample(1400U, heights, upright);
    publish_placement_sample(1500U, heights, upright);
    publish_placement_sample(1600U, heights, upright);
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

static void test_max460_layout_success_failure_reason_retry_and_back(void)
{
    test_ranging_success_and_confirm_preserve_frozen_result();
    test_ranging_failure_reasons_propagate();
    test_ranging_start_error_reports_failure();
    test_ranging_retry_starts_a_new_run_and_ignores_stale_completion();
    test_ranging_back_orphans_run_and_invalidates_late_result();
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

static void test_placement_sample_count_and_calculate_guard(void)
{
    reset_fakes();
    enter_placement();
    const int32_t heights[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_placement_sample(1000U, heights, upright);
    publish_placement_sample(1100U, heights, upright);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(!snapshot.placement.per_node[0].stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);

    publish_placement_sample(1200U, heights, upright);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
    assert(snapshot.placement.per_node_count == 4U);
    assert(snapshot.placement.per_node[0].vertical);
    assert(snapshot.placement.per_node[0].stable);
    assert(snapshot.placement.per_node[1].height_rel_main_mm == 100);
    assert(snapshot.placement.per_node[1].altitude_asl_m == 12.0);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_placement_height_range_50mm_boundary(void)
{
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const int32_t at_boundary[][3] = {
        { 100, 200, 300 },
        { 149, 200, 300 },
        { 150, 200, 300 },
    };
    reset_fakes();
    enter_placement();
    for (size_t i = 0U; i < 3U; ++i) {
        publish_placement_sample(1000U + 100U * i, at_boundary[i], upright);
    }
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].stable);
    assert(snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();

    const int32_t outside_boundary[][3] = {
        { 100, 200, 300 },
        { 149, 200, 300 },
        { 151, 200, 300 },
    };
    reset_fakes();
    enter_placement();
    for (size_t i = 0U; i < 3U; ++i) {
        publish_placement_sample(1000U + 100U * i, outside_boundary[i], upright);
    }
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[1].stable);
    assert(!snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    workflow_service_shutdown();
}

static void test_placement_uses_inclusive_three_second_window(void)
{
    reset_fakes();
    enter_placement();
    const uint8_t upright[3] = { 2U, 2U, 2U };
    const int32_t outside_then_stable[][3] = {
        { 100, 200, 300 },
        { 0, 200, 300 },
        { 0, 200, 300 },
        { 0, 200, 300 },
    };
    publish_placement_sample(1000U, outside_then_stable[0], upright);
    publish_placement_sample(2000U, outside_then_stable[1], upright);
    publish_placement_sample(3000U, outside_then_stable[2], upright);
    publish_placement_sample(3999U, outside_then_stable[3], upright);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[1].stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    publish_placement_sample(4000U, outside_then_stable[3], upright);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[1].stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    publish_placement_sample(4001U, outside_then_stable[3], upright);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[1].stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_layout_rechecks_placement_after_gps_choice(void)
{
    reset_fakes();
    enter_ready_placement();
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED) == WORKFLOW_COMMAND_RESULT_OK);

    const int32_t moved[3] = { 100, 400, 300 };
    const int32_t steady[3] = { 100, 200, 300 };
    const uint8_t upright[3] = { 2U, 2U, 2U };
    publish_placement_sample(1300U, moved, upright);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_CHOICE);
    assert(!snapshot.placement.all_stable);

    assert(command(WORKFLOW_COMMAND_GPS_SKIP) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = get_workflow_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(!snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);

    publish_placement_sample(4301U, steady, upright);
    publish_placement_sample(4401U, steady, upright);
    publish_placement_sample(4501U, steady, upright);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
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
    publish_placement_sample(1000U, heights, upright);
    publish_placement_sample(1100U, heights, upright);
    publish_placement_sample(1200U, heights, upright);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(snapshot.placement.per_node[2].stable);

    publish_placement_sample(1300U, heights, tilted);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].vertical);
    assert(!snapshot.placement.per_node[2].stable);
    assert(!snapshot.placement.all_stable);

    publish_placement_sample(1400U, heights, upright);
    publish_placement_sample(1500U, heights, upright);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].stable);
    publish_placement_sample(1600U, heights, upright);
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
    publish_placement_sample(1000U, heights, upright);
    publish_placement_sample(1100U, heights, upright);
    publish_placement_sample(1200U, heights, upright);
    s_cache[1].sensor.reference_generation++;
    s_now_ms = 1300U;
    s_cache[1].esp_receipt_uptime_ms = s_now_ms;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[2].height_rel_main_mm_valid);
    assert(!snapshot.placement.per_node[2].stable);
    assert(!snapshot.placement.all_stable);

    s_cache[1].sensor.reference_generation = s_requested_generation;
    publish_placement_sample(1400U, heights, upright);
    s_cache[2].response_valid = false;
    s_now_ms = 1500U;
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

static void test_placement_history_overflow_fails_closed(void)
{
    reset_fakes();
    enter_placement();
    const uint8_t upright[3] = { 2U, 2U, 2U };
    for (uint64_t sample = 0U; sample < 40U; ++sample) {
        const int32_t heights[3] = {
            sample == 0U ? 100 : 0,
            200,
            300,
        };
        publish_placement_sample(1000U + sample, heights, upright);
    }
    workflow_snapshot_t snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.per_node[1].stable);
    assert(!snapshot.placement.all_stable);

    const int32_t steady[3] = { 0, 200, 300 };
    publish_placement_sample(4031U, steady, upright);
    snapshot = get_workflow_snapshot();
    assert(!snapshot.placement.all_stable);
    publish_placement_sample(4032U, steady, upright);
    publish_placement_sample(4033U, steady, upright);
    publish_placement_sample(4034U, steady, upright);
    snapshot = get_workflow_snapshot();
    assert(snapshot.placement.all_stable);
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
    test_calibration_cs_pair_invalid_reason_and_node_isolation();
    test_calibration_cs_spread_reason();
    test_calibration_timeout();
    test_calibration_back_cancels_pending_results();
    test_calibration_retry_changes_generation_and_ignores_stale_results();
    test_ranging_start_is_invalidated_and_nonblocking();
    test_max460_layout_success_failure_reason_retry_and_back();
    test_placement_entry_invalidates_back_and_move_sensors();
    test_placement_sample_count_and_calculate_guard();
    test_placement_height_range_50mm_boundary();
    test_placement_uses_inclusive_three_second_window();
    test_layout_rechecks_placement_after_gps_choice();
    test_placement_vertical_flip_resets_sample_window();
    test_placement_generation_mismatch_and_node_dropout_are_unknown();
    test_main_is_vertical_but_needs_a_valid_height_reference();
    test_placement_history_overflow_fails_closed();
    return 0;
}
