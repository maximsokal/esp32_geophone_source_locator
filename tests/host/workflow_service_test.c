#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "freertos/semphr.h"
#include "device_status.h"
#include "main_barometer.h"
#include "measurement_service.h"
#include "nrf_topology_service.h"
#include "object_service.h"
#include "workflow_service.h"

typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} fake_mutex_t;

static measurement_service_snapshot_t s_measurement;
static esp_err_t s_measurement_snapshot_result;
static esp_err_t s_measurement_start_result;
static esp_err_t s_measurement_stop_result;
static esp_err_t s_measurement_delete_result;
static unsigned s_start_calls;
static unsigned s_stop_calls;
static unsigned s_delete_calls;
static object_service_summary_t s_object;
static esp_err_t s_object_result;
static nrf_topology_snapshot_t s_topology;
static esp_err_t s_topology_result;
static nrf_remote_sensor_cache_t s_cache[NRF_TOPOLOGY_REMOTE_COUNT];
static device_status_snapshot_t s_device;
static uint16_t s_height_generation;
static uint64_t s_now_ms;
static bool s_placement_ready;
static nrf_topology_ranging_progress_t s_ranging_progress;

int64_t esp_timer_get_time(void)
{
    return (int64_t)(s_now_ms * UINT64_C(1000));
}

int32_t nrf_baro_pressure_to_altitude_mm(uint32_t pressure_pa)
{
    return (int32_t)(((INT64_C(101325) - pressure_pa) * INT64_C(1000)) / INT64_C(12));
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    fake_mutex_t *mutex = calloc(1U, sizeof(*mutex));
    assert(mutex != NULL);
#ifdef _WIN32
    InitializeCriticalSection(&mutex->lock);
#else
    assert(pthread_mutex_init(&mutex->lock, NULL) == 0);
#endif
    return mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL);
#ifdef _WIN32
    EnterCriticalSection(&mutex->lock);
    return pdTRUE;
#else
    return pthread_mutex_lock(&mutex->lock) == 0 ? pdTRUE : pdFALSE;
#endif
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL);
#ifdef _WIN32
    LeaveCriticalSection(&mutex->lock);
    return pdTRUE;
#else
    return pthread_mutex_unlock(&mutex->lock) == 0 ? pdTRUE : pdFALSE;
#endif
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    fake_mutex_t *mutex = semaphore;
    if (mutex == NULL) return;
#ifdef _WIN32
    DeleteCriticalSection(&mutex->lock);
#else
    assert(pthread_mutex_destroy(&mutex->lock) == 0);
#endif
    free(mutex);
}

esp_err_t measurement_service_get_snapshot(measurement_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_measurement_snapshot_result != ESP_OK) return s_measurement_snapshot_result;
    *snapshot = s_measurement;
    return ESP_OK;
}

esp_err_t measurement_service_start_async(void)
{
    ++s_start_calls;
    if (s_measurement_start_result == ESP_OK) s_measurement.state = MEASUREMENT_SERVICE_STARTING;
    return s_measurement_start_result;
}

esp_err_t measurement_service_stop_async(void)
{
    ++s_stop_calls;
    if (s_measurement_stop_result == ESP_OK) s_measurement.state = MEASUREMENT_SERVICE_STOPPING;
    return s_measurement_stop_result;
}

esp_err_t measurement_service_delete_async(void)
{
    ++s_delete_calls;
    if (s_measurement_delete_result == ESP_OK) s_measurement.state = MEASUREMENT_SERVICE_LIVE;
    return s_measurement_delete_result;
}

esp_err_t object_service_get_active(object_service_summary_t *object)
{
    if (object == NULL) return ESP_ERR_INVALID_ARG;
    if (s_object_result != ESP_OK) return s_object_result;
    *object = s_object;
    return s_object.active ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_topology_result != ESP_OK) return s_topology_result;
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
    for (uint8_t i = 0U; i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        if (s_topology.remotes[i].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            mask |= (uint8_t)(1U << i);
        }
    }
    if (mask == 0U) return ESP_ERR_NOT_FOUND;
    ++s_height_generation;
    if (s_height_generation == 0U) ++s_height_generation;
    *requested_channel_mask = mask;
    *requested_generation = s_height_generation;
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_imu_calibration(uint16_t generation, uint8_t channel_mask)
{
    return generation != 0U && channel_mask != 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
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
    return policy != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nrf_topology_service_ranging_start(const nrf_cs_policy_t *policy, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (policy == NULL) return ESP_ERR_INVALID_ARG;
    memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
    s_ranging_progress.active = true;
    s_ranging_progress.started_at_ms = s_now_ms;
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_get_progress(
    nrf_topology_ranging_progress_t *progress)
{
    if (progress == NULL) return ESP_ERR_INVALID_ARG;
    *progress = s_ranging_progress;
    return ESP_OK;
}

esp_err_t nrf_topology_service_invalidate_ranging(void)
{
    return ESP_OK;
}

esp_err_t main_barometer_request_height_reference_capture(uint16_t generation)
{
    return generation != 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t device_status_try_get_snapshot(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    *snapshot = s_device;
    return ESP_OK;
}

static bool placement_ready(void *context)
{
    (void)context;
    return s_placement_ready;
}

static void reset_fakes(bool active_object, measurement_service_state_t state, size_t assigned)
{
    workflow_service_shutdown();
    memset(&s_measurement, 0, sizeof(s_measurement));
    s_measurement.state = state;
    s_measurement_snapshot_result = ESP_OK;
    s_measurement_start_result = ESP_OK;
    s_measurement_stop_result = ESP_OK;
    s_measurement_delete_result = ESP_OK;
    s_start_calls = 0U;
    s_stop_calls = 0U;
    s_delete_calls = 0U;
    memset(&s_object, 0, sizeof(s_object));
    s_object.active = active_object;
    (void)snprintf(s_object.id, sizeof(s_object.id), "%s", active_object ? "object-a" : "");
    s_object_result = ESP_OK;
    memset(&s_topology, 0, sizeof(s_topology));
    for (size_t i = 0U; i < assigned && i < NRF_TOPOLOGY_REMOTE_COUNT; ++i) {
        s_topology.remotes[i].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        s_topology.remotes[i].node_id = (uint8_t)(i + 1U);
    }
    s_topology_result = ESP_OK;
    memset(s_cache, 0, sizeof(s_cache));
    memset(&s_device, 0, sizeof(s_device));
    s_height_generation = 0U;
    s_now_ms = 1000U;
    s_placement_ready = false;
    memset(&s_ranging_progress, 0, sizeof(s_ranging_progress));
    assert(workflow_service_init() == ESP_OK);
}

static workflow_command_result_t command(workflow_command_t value, bool has_ok, bool ok)
{
    workflow_command_params_t params = {
        .has_ok = has_ok,
        .ok = ok,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    return workflow_service_command(value, &params);
}

static void enter_record_ready(void)
{
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALIBRATION_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_PLACE_SENSORS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_placement_ready = true;
    for (unsigned sample = 0U; sample < 2U; ++sample) {
        s_now_ms += 100U;
        s_device.main.bmp280_valid = true;
        s_device.main.pressure_pa = 101325U;
        s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
        s_device.main.height_reference.generation = s_height_generation;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (s_topology.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
            s_cache[channel].identity_valid = true;
            s_cache[channel].topology_current = true;
            s_cache[channel].response_valid = true;
            s_cache[channel].identity.node_id = s_topology.remotes[channel].node_id;
            s_cache[channel].esp_receipt_uptime_ms = s_now_ms;
            s_cache[channel].sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
            s_cache[channel].sensor.pressure_pa = 101325U;
            s_cache[channel].sensor.orientation_state =
                NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
            s_cache[channel].sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
            s_cache[channel].sensor.reference_generation = s_height_generation;
            s_cache[channel].sensor.relative_z_mm = (int32_t)(100U + channel * 100U);
        }
        assert(workflow_service_poll() == ESP_OK);
    }
    workflow_snapshot_t placement_snapshot;
    assert(workflow_service_get_snapshot(&placement_snapshot) == ESP_OK);
    assert(!placement_snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    s_now_ms += 100U;
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (s_topology.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
            s_cache[channel].esp_receipt_uptime_ms = s_now_ms;
        }
    }
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_service_get_snapshot(&placement_snapshot) == ESP_OK);
    assert(placement_snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_RANGING_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CONFIRM, false, false) == WORKFLOW_COMMAND_RESULT_OK);
}

static void test_reconciliation_review_error_and_async_recording_commands(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    enter_record_ready();
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_RECORD_READY);
    assert(snapshot.sensor_sufficiency == WORKFLOW_SENSOR_CAPABLE_3D);

    const uint32_t ready_generation = snapshot.generation;
    s_measurement_start_result = ESP_ERR_INVALID_STATE;
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_RECORD_READY);
    assert(snapshot.generation == ready_generation);
    assert(s_start_calls == 1U);

    s_measurement_start_result = ESP_OK;
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_start_calls == 2U);
    s_measurement.state = MEASUREMENT_SERVICE_REVIEW;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(snapshot.reason[0] == '\0');

    assert(command(WORKFLOW_COMMAND_RECORD_AGAIN, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_measurement.state = MEASUREMENT_SERVICE_ERROR;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(strcmp(snapshot.reason, "recording_error") == 0);

    assert(command(WORKFLOW_COMMAND_RECORD_AGAIN, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_stop_calls == 1U);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(s_measurement.state == MEASUREMENT_SERVICE_STOPPING);
    s_measurement.state = MEASUREMENT_SERVICE_ERROR;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(strcmp(snapshot.reason, "recording_error") == 0);

    assert(command(WORKFLOW_COMMAND_DELETE_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_delete_calls == 1U);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_RECORD_READY);
    workflow_service_shutdown();
}

static void test_reboot_resumes_only_at_object_or_idle(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_RECORDING, 3U);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_OBJECT);

    workflow_service_shutdown();
    reset_fakes(false, MEASUREMENT_SERVICE_REVIEW, 0U);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_IDLE);
    workflow_service_shutdown();
}

static void test_object_poll_preserves_workflow_on_read_error_and_resets_on_change(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_snapshot_t before;
    assert(workflow_service_get_snapshot(&before) == ESP_OK);
    assert(before.step == WORKFLOW_STEP_SENSOR_CHECK);

    s_object_result = ESP_ERR_INVALID_STATE;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t after;
    assert(workflow_service_get_snapshot(&after) == ESP_OK);
    assert(after.step == WORKFLOW_STEP_SENSOR_CHECK);
    assert(after.generation == before.generation);

    s_object_result = ESP_OK;
    (void)snprintf(s_object.id, sizeof(s_object.id), "%s", "object-b");
    assert(workflow_service_get_snapshot(&after) == ESP_OK);
    assert(after.step == WORKFLOW_STEP_OBJECT);
    assert(after.generation == before.generation + 1U);
    workflow_service_shutdown();
}

typedef struct {
    uint32_t generation;
    workflow_command_result_t result;
} concurrent_call_t;

#ifdef _WIN32
static DWORD WINAPI concurrent_caller(LPVOID context)
#else
static void *concurrent_caller(void *context)
#endif
{
    concurrent_call_t *call = context;
    workflow_command_params_t params = {
        .has_generation = true,
        .expected_generation = call->generation,
    };
    call->result = workflow_service_command(WORKFLOW_COMMAND_START_MEASUREMENT, &params);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_commands_use_generation_as_a_single_winner_gate(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_snapshot_t snapshot;
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    concurrent_call_t calls[2] = {
        { .generation = snapshot.generation },
        { .generation = snapshot.generation },
    };
#ifdef _WIN32
    HANDLE threads[2];
    for (size_t i = 0U; i < 2U; ++i) {
        threads[i] = CreateThread(NULL, 0, concurrent_caller, &calls[i], 0, NULL);
        assert(threads[i] != NULL);
    }
    for (size_t i = 0U; i < 2U; ++i) {
        assert(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[2];
    for (size_t i = 0U; i < 2U; ++i) {
        assert(pthread_create(&threads[i], NULL, concurrent_caller, &calls[i]) == 0);
    }
    for (size_t i = 0U; i < 2U; ++i) assert(pthread_join(threads[i], NULL) == 0);
#endif
    const unsigned winners =
        (calls[0].result == WORKFLOW_COMMAND_RESULT_OK ? 1U : 0U) +
        (calls[1].result == WORKFLOW_COMMAND_RESULT_OK ? 1U : 0U);
    const unsigned stale =
        (calls[0].result == WORKFLOW_COMMAND_RESULT_STALE ? 1U : 0U) +
        (calls[1].result == WORKFLOW_COMMAND_RESULT_STALE ? 1U : 0U);
    assert(winners == 1U && stale == 1U);
    assert(workflow_service_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
    assert(snapshot.generation == calls[0].generation + 1U);
    workflow_service_shutdown();
}

int main(void)
{
    test_reconciliation_review_error_and_async_recording_commands();
    test_reboot_resumes_only_at_object_or_idle();
    test_object_poll_preserves_workflow_on_read_error_and_resets_on_change();
    test_concurrent_commands_use_generation_as_a_single_winner_gate();
    return 0;
}
