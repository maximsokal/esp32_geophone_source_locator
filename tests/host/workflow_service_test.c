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
#include "object_store.h"
#include "workflow_service.h"
#include "workflow_service_host_fakes.h"
#include "cJSON.h"

#ifndef CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING
#define CONFIG_RECORDING_GATE_REQUIRE_REMOTES_AND_LEVELING 0
#endif

/* The workflow service host stub omits the unrelated status-only leveling API. */
typedef struct {
    size_t assigned_count;
    size_t accepted_count;
} nrf_topology_leveling_summary_t;

typedef enum {
    NRF_TOPOLOGY_LEVELING_NO_DATA = 0,
    NRF_TOPOLOGY_LEVELING_UNAVAILABLE,
    NRF_TOPOLOGY_LEVELING_MOVING,
    NRF_TOPOLOGY_LEVELING_ACCEPTED,
    NRF_TOPOLOGY_LEVELING_OUTSIDE,
} nrf_topology_leveling_node_state_t;

typedef struct {
    uint8_t channel;
    uint8_t node_id;
    nrf_topology_leveling_node_state_t state;
    bool tilt_valid;
    float tilt_deg;
} nrf_topology_leveling_node_t;

typedef struct {
    size_t count;
    nrf_topology_leveling_node_t nodes[8];
} nrf_topology_leveling_nodes_t;

esp_err_t nrf_topology_service_get_leveling_summary(nrf_topology_leveling_summary_t *summary);
esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes);
const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state);

/* Exercise the production status serializer against this test's live service. */
#undef ESP_FAIL
#define ESP_FAIL -1
#define ESP_LOGE(tag, format, ...) ((void)0)
#include "../../components/web_server/web_status_json_recording.c"
#undef ESP_LOGE
#undef ESP_FAIL
#define ESP_FAIL (-1)


static const char s_deep_nested_settings_json[] =
    "{\"propagation_speed_mps\":340,\"propagation_speed_uncertainty_mps\":5,"
    "\"timing_uncertainty_us\":2000,\"noise_window_samples\":50,"
    "\"sta_window_samples\":5,\"lta_window_samples\":50,"
    "\"sta_lta_trigger_ratio\":3.5,\"refine_lookback_samples\":10,"
    "\"refine_lookahead_samples\":10,\"refine_amplitude_fraction\":0.5,"
    "\"snr_signal_window_samples\":50,\"minimum_snr\":2,"
    "\"semblance_window_half_span_samples\":25,"
    "\"unknown\":{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":0}}}}}}}}}";

static const char s_concrete_settings_json[] =
    "{\"material\":\"concrete\",\"propagation_speed_mps\":4000,"
    "\"propagation_speed_uncertainty_mps\":500,\"propagation_model_name\":\"site-model\","
    "\"timing_uncertainty_us\":2000,\"noise_window_samples\":50,"
    "\"sta_window_samples\":10,\"lta_window_samples\":100,\"sta_lta_trigger_ratio\":3,"
    "\"refine_lookback_samples\":10,\"refine_lookahead_samples\":10,"
    "\"refine_amplitude_fraction\":0.5,\"snr_signal_window_samples\":20,"
    "\"minimum_snr\":3,\"semblance_window_half_span_samples\":10,"
    "\"s_pick_enabled\":false,\"time_budget_s\":60}";

typedef struct {
#ifdef _WIN32
    SRWLOCK lock;
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
static const char *s_fake_object_id = "object-a";
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
    InitializeSRWLock(&mutex->lock);
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
    if (timeout == 0U) return TryAcquireSRWLockExclusive(&mutex->lock) ? pdTRUE : pdFALSE;
    AcquireSRWLockExclusive(&mutex->lock);
    return pdTRUE;
#else
    if (timeout == 0U) return pthread_mutex_trylock(&mutex->lock) == 0 ? pdTRUE : pdFALSE;
    return pthread_mutex_lock(&mutex->lock) == 0 ? pdTRUE : pdFALSE;
#endif
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    fake_mutex_t *mutex = semaphore;
    assert(mutex != NULL);
#ifdef _WIN32
    ReleaseSRWLockExclusive(&mutex->lock);
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
    (void)mutex;
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
    if (s_measurement_start_result == ESP_OK) {
        s_measurement.state = MEASUREMENT_SERVICE_STARTING;
        (void)snprintf(s_measurement.object_id, sizeof(s_measurement.object_id), "%s", s_object.id);
        (void)snprintf(s_measurement.session_id, sizeof(s_measurement.session_id), "%s", "session-1");
    }
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

esp_err_t nrf_topology_service_get_leveling_summary(
    nrf_topology_leveling_summary_t *summary
)
{
    (void)summary;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t nrf_topology_service_get_leveling_nodes(nrf_topology_leveling_nodes_t *nodes)
{
    (void)nodes;
    return ESP_ERR_INVALID_STATE;
}

const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state)
{
    switch (state) {
        case NRF_TOPOLOGY_LEVELING_NO_DATA: return "no_data";
        case NRF_TOPOLOGY_LEVELING_UNAVAILABLE: return "unavailable";
        case NRF_TOPOLOGY_LEVELING_MOVING: return "moving";
        case NRF_TOPOLOGY_LEVELING_ACCEPTED: return "accepted";
        case NRF_TOPOLOGY_LEVELING_OUTSIDE: return "outside";
        default: return "unknown";
    }
}

esp_err_t seismic_recorder_get_snapshot(seismic_recorder_snapshot_t *snapshot)
{
    (void)snapshot;
    return ESP_ERR_INVALID_STATE;
}

const char *seismic_recorder_stop_stage_name(seismic_recorder_stop_stage_t stage)
{
    (void)stage;
    return "NONE";
}

bool storage_manager_is_ready(void)
{
    return true;
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
    workflow_service_host_fakes_reset();
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
    (void)snprintf(s_object.id, sizeof(s_object.id), "%s", active_object ? s_fake_object_id : "");
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

extern void workflow_service_test_lock_mutex(void);
extern void workflow_service_test_unlock_mutex(void);

static workflow_snapshot_t try_snapshot(void)
{
    workflow_snapshot_t snapshot;
    assert(workflow_service_try_get_snapshot(&snapshot) == ESP_OK);
    return snapshot;
}

static void assert_workflow_json_matches_snapshot(const workflow_snapshot_t *expected)
{
    cJSON *root = cJSON_CreateObject();
    assert(root != NULL);
    assert(web_status_json_add_workflow(root));
    const cJSON *workflow = cJSON_GetObjectItemCaseSensitive(root, "workflow");
    assert(cJSON_IsObject(workflow));

    const cJSON *step = cJSON_GetObjectItemCaseSensitive(workflow, "step");
    assert(cJSON_IsString(step));
    assert(strcmp(step->valuestring, workflow_core_step_name(expected->step)) == 0);

    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(workflow, "generation");
    assert(cJSON_IsNumber(generation));
    assert(generation->valuedouble == (double)expected->generation);

    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(workflow, "allowed_actions");
    assert(cJSON_IsArray(actions));
    uint32_t serialized_actions = 0U;
    const int action_count = cJSON_GetArraySize(actions);
    for (int index = 0; index < action_count; ++index) {
        const cJSON *action = cJSON_GetArrayItem(actions, index);
        workflow_command_t command_value;
        assert(cJSON_IsString(action));
        assert(workflow_core_command_from_name(action->valuestring, &command_value));
        serialized_actions |= WORKFLOW_ACTION(command_value);
    }
    assert(serialized_actions == expected->allowed_actions);
    cJSON_Delete(root);
}

static void test_phone_and_tft_workflow_views_share_step_and_generation(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    const workflow_snapshot_t initial = try_snapshot();
    assert(initial.step == WORKFLOW_STEP_OBJECT);

    /* This is the public service call made by /api/control's workflow handler. */
    const workflow_command_params_t phone_params = {
        .has_generation = true,
        .expected_generation = initial.generation,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    assert(workflow_service_command(WORKFLOW_COMMAND_START_MEASUREMENT, &phone_params) ==
        WORKFLOW_COMMAND_RESULT_OK);
    const workflow_snapshot_t phone_snapshot = try_snapshot();
    assert(phone_snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
    assert(phone_snapshot.generation == initial.generation + 1U);
    assert((phone_snapshot.allowed_actions &
        WORKFLOW_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION)) != 0U);

    uint32_t tft_sequence = 0U;
    assert(workflow_service_command_async(
        WORKFLOW_COMMAND_GO_CALIBRATION,
        true,
        phone_snapshot.generation,
        &tft_sequence
    ) == ESP_OK);
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t tft_snapshot = try_snapshot();
    assert(tft_snapshot.step == WORKFLOW_STEP_CALIB_PREP);
    assert(tft_snapshot.generation == phone_snapshot.generation + 1U);
    assert(tft_snapshot.last_async.seq == tft_sequence);
    assert(tft_snapshot.last_async.command == WORKFLOW_COMMAND_GO_CALIBRATION);
    assert(tft_snapshot.last_async.result == WORKFLOW_COMMAND_RESULT_OK);
    assert((tft_snapshot.allowed_actions &
        WORKFLOW_ACTION(WORKFLOW_COMMAND_SENSORS_PLACED)) != 0U);
    assert_workflow_json_matches_snapshot(&tft_snapshot);

    const workflow_command_params_t stale_phone_params = {
        .has_generation = true,
        .expected_generation = phone_snapshot.generation,
        .calculation_class = WORKFLOW_CALCULATION_CLASS_NONE,
    };
    assert(workflow_service_command(WORKFLOW_COMMAND_BACK, &stale_phone_params) ==
        WORKFLOW_COMMAND_RESULT_STALE);
    const workflow_snapshot_t after_stale = try_snapshot();
    assert(after_stale.step == tft_snapshot.step);
    assert(after_stale.generation == tft_snapshot.generation);
    assert(after_stale.allowed_actions == tft_snapshot.allowed_actions);
    assert_workflow_json_matches_snapshot(&after_stale);
    workflow_service_shutdown();
}

static void enter_stopped(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    enter_record_ready();
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(workflow_service_poll() == ESP_OK);
    assert(strcmp(try_snapshot().session_id, "session-1") == 0);
    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_measurement.state = MEASUREMENT_SERVICE_REVIEW;
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(strcmp(snapshot.object_id, "object-a") == 0);
    assert(strcmp(snapshot.session_id, "session-1") == 0);
}

static uint32_t begin_calculation(void)
{
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(snapshot.calc_settings_ready);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES)) != 0U);
    assert(command(WORKFLOW_COMMAND_CALCULATE_SOURCES, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALCULATING);
    assert(workflow_test_job_submit_calls == 1U);
    assert(strcmp(workflow_test_submitted_object_id, "object-a") == 0);
    assert(strcmp(workflow_test_submitted_session_id, "session-1") == 0);
    return workflow_test_job_status.job_id;
}

static void set_job_result(localization_recommendation_action_t action, bool successful)
{
    workflow_test_job_status.state = successful ? LOCALIZATION_JOB_SUCCEEDED : LOCALIZATION_JOB_FAILED;
    workflow_test_job_status.error = ESP_FAIL;
    workflow_test_job_status.summary.recommendation.action = action;
    workflow_test_job_status.summary.recommendation.successful = successful;
    (void)snprintf(workflow_test_job_status.summary.recommendation.reason_summary,
        sizeof(workflow_test_job_status.summary.recommendation.reason_summary), "%s", "test_classification");
}

static void test_try_snapshot_is_nonblocking_while_mutex_is_held(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_snapshot_t snapshot;
    workflow_service_test_lock_mutex();
    assert(workflow_service_try_get_snapshot(&snapshot) == ESP_ERR_TIMEOUT);
    workflow_service_test_unlock_mutex();
    assert(workflow_service_try_get_snapshot(&snapshot) == ESP_OK);
    workflow_service_shutdown();
}

static void test_async_fifo_stale_generation_full_queue_and_monotonic_sequence(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_snapshot_t snapshot = try_snapshot();
    uint32_t seq1 = 0U;
    uint32_t seq2 = 0U;
    assert(workflow_service_command_async(WORKFLOW_COMMAND_START_MEASUREMENT,
        true, snapshot.generation, &seq1) == ESP_OK);
    assert(workflow_service_command_async(WORKFLOW_COMMAND_GO_CALIBRATION,
        true, snapshot.generation + 1U, &seq2) == ESP_OK);
    assert(seq2 > seq1);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALIB_PREP);
    assert(snapshot.last_async.seq == seq2);
    assert(snapshot.last_async.command == WORKFLOW_COMMAND_GO_CALIBRATION);
    assert(snapshot.last_async.result == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    snapshot = try_snapshot();
    uint32_t stale_seq = 0U;
    assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK,
        true, snapshot.generation + 9U, &stale_seq) == ESP_OK);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.last_async.seq == stale_seq);
    assert(snapshot.last_async.result == WORKFLOW_COMMAND_RESULT_STALE);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    uint32_t accepted[4] = {0};
    for (size_t i = 0U; i < 4U; ++i) {
        assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK,
            false, 0U, &accepted[i]) == ESP_OK);
        if (i > 0U) assert(accepted[i] > accepted[i - 1U]);
    }
    uint32_t unchanged = UINT32_C(0x12345678);
    assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK,
        false, 0U, &unchanged) == ESP_ERR_NO_MEM);
    assert(unchanged == UINT32_C(0x12345678));
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.last_async.seq == accepted[3]);
    workflow_service_shutdown();
}

static void test_async_rejects_internal_completion_commands(void)
{
    static const workflow_command_t internal_commands[] = {
        WORKFLOW_COMMAND_CALIBRATION_DONE,
        WORKFLOW_COMMAND_RANGING_DONE,
        WORKFLOW_COMMAND_CALCULATION_DONE,
    };
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    const workflow_snapshot_t before = try_snapshot();
    uint32_t first_seq = 0U;
    assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK, false, 0U, &first_seq) == ESP_OK);
    for (size_t i = 0U; i < sizeof(internal_commands) / sizeof(internal_commands[0]); ++i) {
        uint32_t seq = UINT32_C(0x12345678);
        assert(workflow_service_command_async(
            internal_commands[i], false, 0U, &seq) == ESP_ERR_INVALID_ARG);
        assert(seq == UINT32_C(0x12345678));
    }

    uint32_t next_seq = 0U;
    assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK, false, 0U, &next_seq) == ESP_OK);
    uint32_t expected_seq = first_seq + 1U;
    if (expected_seq == 0U) expected_seq = 1U;
    assert(next_seq == expected_seq);
    const workflow_snapshot_t after = try_snapshot();
    assert(after.step == before.step && after.generation == before.generation);
    assert(before.step == WORKFLOW_STEP_OBJECT);
    workflow_service_shutdown();
}

static void test_periodic_poll_alone_advances_calibration_timeout(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(try_snapshot().step == WORKFLOW_STEP_CALIBRATING);
    s_now_ms += UINT64_C(15000);
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && !snapshot.calibration.result_ok);
    bool timeout_reason = false;
    for (size_t i = 0U; i < snapshot.calibration.reasons_count; ++i) {
        timeout_reason = timeout_reason || strcmp(snapshot.calibration.reasons[i], "timeout") == 0;
    }
    assert(timeout_reason);
    workflow_service_shutdown();
}


static bool snapshot_has_calibration_reason(
    const workflow_snapshot_t *snapshot,
    const char *expected)
{
    for (size_t i = 0U; i < snapshot->calibration.reasons_count; ++i) {
        if (strcmp(snapshot->calibration.reasons[i], expected) == 0) return true;
    }
    return false;
}


static void test_low_remote_calibration_timeout_and_pair_counts(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 0U);
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION));
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && !snapshot.calibration.result_ok);
    assert(snapshot_has_calibration_reason(&snapshot, "insufficient_nodes"));
    workflow_service_shutdown();

    for (size_t remotes = 1U; remotes < 3U; ++remotes) {
        reset_fakes(true, MEASUREMENT_SERVICE_LIVE, remotes);
        assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        snapshot = try_snapshot();
        assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_GO_CALIBRATION)) != 0U);
        assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_CALIBRATING);
        assert(snapshot.calibration.pairs_total == remotes);
        s_now_ms += UINT64_C(15000);
        assert(workflow_service_poll() == ESP_OK);
        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
        assert(snapshot.calibration.result_valid && !snapshot.calibration.result_ok);
        assert(snapshot_has_calibration_reason(&snapshot, "timeout"));
        workflow_service_shutdown();
    }
}


static void test_low_remote_placement_and_ranging_finish(void)
{
    for (size_t remotes = 1U; remotes <= 2U; ++remotes) {
        reset_fakes(true, MEASUREMENT_SERVICE_LIVE, remotes);
        assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
        assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_CALIBRATION_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_PLACE_SENSORS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        s_placement_ready = true;
        for (unsigned sample = 0U; sample < WORKFLOW_PLACEMENT_MIN_SAMPLES; ++sample) {
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
            }
            assert(workflow_service_poll() == ESP_OK);
        }
        workflow_snapshot_t snapshot = try_snapshot();
        assert(snapshot.placement.all_stable);
        assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);

        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
        assert(strcmp(snapshot.reason, "insufficient_nodes") == 0);
        assert(snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY));
        workflow_service_shutdown();
    }
}

static void test_scan_presence_and_settings_refresh_are_cached_for_two_seconds(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_scan_present = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_test_scan_calls == 1U);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.has_scan && snapshot.calc_settings_ready);
    assert(workflow_test_scan_calls == 1U);
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_test_scan_calls == 1U);
    s_now_ms += UINT64_C(1999);
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_test_scan_calls == 1U);
    s_now_ms += 1U;
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_test_scan_calls == 2U);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_scan_present = true; /* no session yet: represents the staged scan */
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.session_id[0] == '\0' && snapshot.has_scan);
    workflow_service_shutdown();
}

static void test_calc_settings_readiness_gates_calculate_and_save_invalidates_cache(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = NULL;
    assert(workflow_service_poll() == ESP_OK);
    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    enter_record_ready();
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(workflow_service_poll() == ESP_OK);
    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_measurement.state = MEASUREMENT_SERVICE_REVIEW;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED && !snapshot.calc_settings_ready);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_CALCULATE_SOURCES)) == 0U);
    assert(command(WORKFLOW_COMMAND_CALCULATE_SOURCES, false, false) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(workflow_test_job_submit_calls == 0U);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = "{";
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(!snapshot.calc_settings_ready);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = s_deep_nested_settings_json;
    assert(workflow_service_poll() == ESP_OK);
    assert(!try_snapshot().calc_settings_ready);
    workflow_service_shutdown();

    enter_stopped();
    assert(workflow_service_notify_calc_settings_saved("object-a") == ESP_OK);
    assert(!try_snapshot().calc_settings_ready);
    assert(workflow_service_poll() == ESP_OK);
    assert(try_snapshot().calc_settings_ready);
    workflow_service_shutdown();
}

static void test_calculation_success_progress_failure_classes_and_submit_errors(void)
{
    enter_stopped();
    uint32_t job_id = begin_calculation();
    workflow_test_job_status.state = LOCALIZATION_JOB_RUNNING;
    workflow_test_job_status.progress_percent = 37U;
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALCULATING && snapshot.progress == 37);
    set_job_result(LOCALIZATION_RECOMMENDATION_NONE, true);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RESULT);
    assert(snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_NONE);
    assert(strcmp(snapshot.object_id, "object-a") == 0 && strcmp(snapshot.session_id, "session-1") == 0);
    workflow_service_shutdown();
    (void)job_id;

    const struct {
        localization_recommendation_action_t action;
        workflow_calculation_class_t expected;
    } failures[] = {
        {LOCALIZATION_RECOMMENDATION_RECORD_AGAIN, WORKFLOW_CALCULATION_CLASS_RECORD_AGAIN},
        {LOCALIZATION_RECOMMENDATION_MOVE_SENSORS, WORKFLOW_CALCULATION_CLASS_MOVE_SENSORS},
        {LOCALIZATION_RECOMMENDATION_NONE, WORKFLOW_CALCULATION_CLASS_INTERNAL},
    };
    for (size_t i = 0U; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        enter_stopped();
        (void)begin_calculation();
        set_job_result(failures[i].action, false);
        assert(workflow_service_poll() == ESP_OK);
        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_RESULT);
        assert(snapshot.calculation_class == failures[i].expected);
        assert(strcmp(snapshot.reason, "ESP_FAIL") == 0);
        workflow_service_shutdown();
    }

    enter_stopped();
    workflow_test_job_submit_result = ESP_ERR_INVALID_STATE;
    (void)command(WORKFLOW_COMMAND_CALCULATE_SOURCES, false, false);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RESULT);
    assert(snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_INTERNAL);
    assert(strcmp(snapshot.reason, "submit_failed:ESP_ERR_INVALID_STATE") == 0);
    assert(workflow_test_job_submit_calls == 1U);
    workflow_service_shutdown();
}

static void test_invalid_settings_at_submit_and_orphaned_job_completion(void)
{
    enter_stopped();
    assert(try_snapshot().calc_settings_ready);
    workflow_test_settings_json = s_deep_nested_settings_json;
    (void)command(WORKFLOW_COMMAND_CALCULATE_SOURCES, false, false);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RESULT);
    assert(snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_INTERNAL);
    assert(strcmp(snapshot.reason, "calc_settings_invalid") == 0);
    assert(workflow_test_job_submit_calls == 0U);
    workflow_service_shutdown();

    enter_stopped();
    const uint32_t job_id = begin_calculation();
    assert(command(WORKFLOW_COMMAND_BACK, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(workflow_test_job_status.job_id == job_id);
    set_job_result(LOCALIZATION_RECOMMENDATION_NONE, true);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(workflow_test_job_submit_calls == 1U);
    workflow_service_shutdown();
}

static void test_second_calculation_while_orphaned_is_busy_without_duplicate_submit(void)
{
    enter_stopped();
    (void)begin_calculation();
    assert(command(WORKFLOW_COMMAND_BACK, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(workflow_test_job_status.state == LOCALIZATION_JOB_QUEUED);
    assert(command(WORKFLOW_COMMAND_CALCULATE_SOURCES, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    const workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RESULT);
    assert(snapshot.calculation_class == WORKFLOW_CALCULATION_CLASS_INTERNAL);
    assert(strcmp(snapshot.reason, "busy") == 0);
    assert(workflow_test_job_submit_calls == 1U);
    workflow_service_shutdown();
}

static void test_material_save_runs_in_poll_and_refreshes_settings_ready(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = NULL;
    assert(!try_snapshot().calc_settings_ready);
    assert(workflow_service_save_calc_material_async("custom", 50U, 1U) == ESP_ERR_INVALID_ARG);
    assert(workflow_service_save_calc_material_async("custom", 340U, 340U) == ESP_ERR_INVALID_ARG);
    assert(workflow_service_save_calc_material_async("unknown", 340U, 5U) == ESP_ERR_INVALID_ARG);
    workflow_service_test_lock_mutex();
    assert(workflow_service_save_calc_material_async("concrete", 0U, 0U) == ESP_OK);
    workflow_service_test_unlock_mutex();
    assert(workflow_test_settings_save_calls == 0U);
    assert(try_snapshot().last_material_save.seq == 0U);

    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(workflow_test_settings_save_calls == 1U);
    assert(strcmp(workflow_test_saved_object_id, "object-a") == 0);
    assert(strstr(workflow_test_saved_settings, "\"material\":\"concrete\"") != NULL);
    assert(snapshot.last_material_save.seq != 0U);
    assert(snapshot.last_material_save.result == ESP_OK);
    assert(snapshot.calc_settings_ready);
    assert(strcmp(snapshot.calc_material, "concrete") == 0);
    workflow_service_shutdown();
}

static void test_material_save_reports_sd_error_and_missing_active_object(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = NULL;
    workflow_test_settings_save_result = ESP_FAIL;
    assert(workflow_service_save_calc_material_async("soil_dry", 0U, 0U) == ESP_OK);
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(workflow_test_settings_save_calls == 1U);
    assert(snapshot.last_material_save.seq != 0U);
    assert(snapshot.last_material_save.result == ESP_FAIL);
    assert(!snapshot.calc_settings_ready);
    workflow_service_shutdown();

    reset_fakes(false, MEASUREMENT_SERVICE_LIVE, 0U);
    assert(workflow_service_save_calc_material_async("concrete", 0U, 0U) == ESP_OK);
    assert(workflow_test_settings_save_calls == 0U);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(workflow_test_settings_save_calls == 0U);
    assert(snapshot.last_material_save.seq != 0U);
    assert(snapshot.last_material_save.result == ESP_ERR_NOT_FOUND);
    assert(snapshot.calc_material[0] == '\0');
    workflow_service_shutdown();
}

static void test_material_save_queue_full_is_rejected_without_sd_write(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    for (size_t i = 0U; i < 4U; ++i) {
        assert(workflow_service_save_calc_material_async("rubble", 0U, 0U) == ESP_OK);
    }
    assert(workflow_service_save_calc_material_async("steel", 0U, 0U) == ESP_ERR_NO_MEM);
    assert(workflow_test_settings_save_calls == 0U);
    assert(workflow_service_poll() == ESP_OK);
    const workflow_snapshot_t snapshot = try_snapshot();
    assert(workflow_test_settings_save_calls == 4U);
    assert(snapshot.last_material_save.seq != 0U);
    assert(snapshot.last_material_save.result == ESP_OK);
    assert(strcmp(snapshot.calc_material, "rubble") == 0);
    workflow_service_shutdown();
}

/* MAX-312 (new behaviour; the range API did not exist before): a TFT custom
 * range is persisted as a user velocity_range and reported back in the
 * snapshot; "auto" persists no explicit range, so the device derives it. */
static void test_tft_custom_range_is_persisted_and_auto_omits_it(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = NULL;
    assert(workflow_service_save_calc_material_range_async("custom", 1000U, 200U, true, 300U, 3000U) == ESP_OK);
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(workflow_test_settings_save_calls == 1U);
    assert(snapshot.last_material_save.result == ESP_OK);
    assert(strstr(workflow_test_saved_settings, "\"material\":\"custom\"") != NULL);
    assert(strstr(workflow_test_saved_settings, "\"velocity_range\"") != NULL);
    assert(strstr(workflow_test_saved_settings, "\"source\":\"user\"") != NULL);
    assert(strstr(workflow_test_saved_settings, "\"v_min_mps\":300") != NULL);
    assert(strstr(workflow_test_saved_settings, "\"v_max_mps\":3000") != NULL);
    assert(snapshot.calc_settings_ready);
    assert(snapshot.calc_velocity_range_set);
    assert(snapshot.calc_velocity_range_min_mps == 300.0f);
    assert(snapshot.calc_velocity_range_max_mps == 3000.0f);
    workflow_service_shutdown();

    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_settings_json = NULL;
    assert(workflow_service_save_calc_material_range_async("custom", 1000U, 200U, false, 0U, 0U) == ESP_OK);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.last_material_save.result == ESP_OK);
    assert(strstr(workflow_test_saved_settings, "\"material\":\"custom\"") != NULL);
    assert(strstr(workflow_test_saved_settings, "velocity_range") == NULL);
    assert(snapshot.calc_settings_ready);
    assert(!snapshot.calc_velocity_range_set);
    workflow_service_shutdown();
}


static void test_material_is_carried_into_run_provenance(void);


static void test_material_is_carried_into_run_provenance(void)
{
    enter_stopped();
    workflow_test_settings_json = s_concrete_settings_json;
    (void)begin_calculation();
    assert(workflow_test_submitted_config.propagation_speed_mps == 4000.0);
    assert(strcmp(workflow_test_submitted_config.propagation_model_name, "site-model") == 0);
    assert(strcmp(workflow_test_submitted_config.material, "concrete") == 0);
    workflow_service_shutdown();
}

int main(void)
{
#ifdef MAX449_CASE
#if MAX449_CASE == 4
    test_low_remote_calibration_timeout_and_pair_counts();
    test_low_remote_placement_and_ranging_finish();
#else
#error "unknown MAX449_CASE for workflow_service_test"
#endif
#else
    test_phone_and_tft_workflow_views_share_step_and_generation();
    test_tft_custom_range_is_persisted_and_auto_omits_it();
    test_reconciliation_review_error_and_async_recording_commands();
    test_reboot_resumes_only_at_object_or_idle();
    test_object_poll_preserves_workflow_on_read_error_and_resets_on_change();
    test_concurrent_commands_use_generation_as_a_single_winner_gate();
    test_try_snapshot_is_nonblocking_while_mutex_is_held();
    test_async_fifo_stale_generation_full_queue_and_monotonic_sequence();
    test_async_rejects_internal_completion_commands();
    test_periodic_poll_alone_advances_calibration_timeout();
    test_scan_presence_and_settings_refresh_are_cached_for_two_seconds();
    test_calc_settings_readiness_gates_calculate_and_save_invalidates_cache();
    test_calculation_success_progress_failure_classes_and_submit_errors();
    test_invalid_settings_at_submit_and_orphaned_job_completion();
    test_second_calculation_while_orphaned_is_busy_without_duplicate_submit();
    test_material_save_runs_in_poll_and_refreshes_settings_ready();
    test_material_save_reports_sd_error_and_missing_active_object();
    test_material_save_queue_full_is_rejected_without_sd_write();
    test_material_is_carried_into_run_provenance();
    return 0;
#endif
}
