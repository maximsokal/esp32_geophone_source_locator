#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <math.h>
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
#include "app_stop_timeout_config.h"
#include "workflow_service_host_fakes.h"
#include "host_rtos.h"
#include "cJSON.h"
#include "esp_log.h"

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
    volatile long held;
} fake_mutex_t;

typedef struct {
    unsigned count;
} fake_binary_semaphore_t;

static fake_binary_semaphore_t *s_poll_stop_ack;
static unsigned s_host_semaphore_delete_calls;
static unsigned s_poll_ack_give_calls;
static TickType_t s_poll_ack_last_timeout;
#ifdef _WIN32
static SRWLOCK s_poll_barrier_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_poll_barrier_changed = CONDITION_VARIABLE_INIT;
#else
static pthread_mutex_t s_poll_barrier_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_poll_barrier_changed = PTHREAD_COND_INITIALIZER;
#endif
static bool s_poll_barrier_enabled;
static bool s_poll_barrier_entered;
static bool s_poll_barrier_released;
static TickType_t s_next_poll_work_ticks;

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
static unsigned s_height_request_calls;
static unsigned s_barometer_polling_requests;
static unsigned s_imu_request_calls;
static unsigned s_cs_request_calls;
static uint8_t s_imu_request_mask;
static uint16_t s_imu_request_generation;
static bool s_cs_cycle_valid;
static nrf_cs_cycle_snapshot_t s_cs_cycle;
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

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    s_poll_stop_ack = calloc(1U, sizeof(*s_poll_stop_ack));
    assert(s_poll_stop_ack != NULL);
    return s_poll_stop_ack;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    if (semaphore == s_poll_stop_ack) {
        fake_binary_semaphore_t *binary = semaphore;
        s_poll_ack_last_timeout = timeout;
        if (
            binary != NULL && binary->count == 0U &&
            !workflow_host_rtos_task_thread_running()
        ) {
            workflow_host_rtos_run_created_task_sync();
        }
        if (binary == NULL || binary->count == 0U) return pdFALSE;
        --binary->count;
        return pdTRUE;
    }
    (void)timeout;
    fake_mutex_t *mutex = semaphore;
    if (mutex == NULL) return pdFALSE;
#ifdef _WIN32
    if (timeout == 0U) {
        if (!TryAcquireSRWLockExclusive(&mutex->lock)) return pdFALSE;
        (void)InterlockedExchange(&mutex->held, 1L);
        return pdTRUE;
    }
    AcquireSRWLockExclusive(&mutex->lock);
    (void)InterlockedExchange(&mutex->held, 1L);
    return pdTRUE;
#else
    const int locked = timeout == 0U
        ? pthread_mutex_trylock(&mutex->lock)
        : pthread_mutex_lock(&mutex->lock);
    if (locked != 0) return pdFALSE;
    mutex->held = 1L;
    return pdTRUE;
#endif
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == s_poll_stop_ack) {
        fake_binary_semaphore_t *binary = semaphore;
        if (binary == NULL) return pdFALSE;
        ++binary->count;
        ++s_poll_ack_give_calls;
        return pdTRUE;
    }
    fake_mutex_t *mutex = semaphore;
    if (mutex == NULL) return pdFALSE;
#ifdef _WIN32
    (void)InterlockedExchange(&mutex->held, 0L);
    ReleaseSRWLockExclusive(&mutex->lock);
    return pdTRUE;
#else
    mutex->held = 0L;
    return pthread_mutex_unlock(&mutex->lock) == 0 ? pdTRUE : pdFALSE;
#endif
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    ++s_host_semaphore_delete_calls;
    if (semaphore == s_poll_stop_ack) {
        free(semaphore);
        s_poll_stop_ack = NULL;
        return;
    }
    fake_mutex_t *mutex = semaphore;
    if (mutex == NULL) return;
    if (mutex->held != 0L) return;
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
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_poll_barrier_lock) == 0);
#endif
    if (s_poll_barrier_enabled) {
        s_poll_barrier_entered = true;
#ifdef _WIN32
        WakeAllConditionVariable(&s_poll_barrier_changed);
        while (!s_poll_barrier_released) {
            assert(SleepConditionVariableSRW(
                &s_poll_barrier_changed, &s_poll_barrier_lock, INFINITE, 0U));
        }
#else
        assert(pthread_cond_broadcast(&s_poll_barrier_changed) == 0);
        while (!s_poll_barrier_released) {
            assert(pthread_cond_wait(&s_poll_barrier_changed, &s_poll_barrier_lock) == 0);
        }
#endif
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_unlock(&s_poll_barrier_lock) == 0);
#endif
    if (s_next_poll_work_ticks > 0U) {
        workflow_host_rtos_advance_ticks(s_next_poll_work_ticks);
        s_next_poll_work_ticks = 0U;
    }
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
    ++s_height_request_calls;
    ++s_height_generation;
    if (s_height_generation == 0U) ++s_height_generation;
    *requested_channel_mask = mask;
    *requested_generation = s_height_generation;
    return ESP_OK;
}

esp_err_t nrf_topology_service_request_imu_calibration(uint16_t generation, uint8_t channel_mask)
{
    if (generation == 0U || channel_mask == 0U) return ESP_ERR_INVALID_ARG;
    ++s_imu_request_calls;
    s_imu_request_mask = channel_mask;
    s_imu_request_generation = generation;
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
    if (policy == NULL) return ESP_ERR_INVALID_ARG;
    ++s_cs_request_calls;
    return ESP_OK;
}

esp_err_t nrf_topology_service_get_cs_cycle(nrf_cs_cycle_snapshot_t *snapshot)
{
    if (!s_cs_cycle_valid) return ESP_ERR_INVALID_STATE;
    *snapshot = s_cs_cycle;
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_start_autonomous_limited(const nrf_cs_policy_t *policy, const uint32_t *limits)
{
    assert(limits != NULL && limits[0] == 5000U && limits[3] == 5000U && limits[4] == 0U);
    return nrf_topology_service_ranging_start_autonomous(policy);
}

esp_err_t nrf_topology_service_ranging_start_autonomous(const nrf_cs_policy_t *policy)
{
    if (policy == NULL) return ESP_ERR_INVALID_ARG;
    /* The pair time budget measured on the bench (see s_workflow_ranging_policy): 2.4 s, nine samples per direction, one spare try. */
    assert(policy->pair_timeout_ms == 2400U && policy->max_attempts == 2U);
    assert(policy->samples_per_pair == 9U && policy->full_rotation);
    /* The ESP32 holds the I2C bus silent for the whole cycle and sensor_bus_quiet_begin() refuses more than
     * SENSOR_BUS_QUIET_MAX_MS (600000). The production network is MAIN + 4 nodes: 5 nodes, 20 directed executions. A longer
     * window per execution (more samples, a retry budget) that pushes this past the limit makes the cycle unstartable
     * ("bus_unavailable") exactly on the real hardware, and a bench with 3 or 4 nodes would not show it. */
    /* The budget formula of nrf_cs_cycle_coordinator_budget_ms (pinned in nrf_cs_cycle_coordinator_test): per execution
     * 300 ms GO skew + (samples + attempts - 1) * (budget 2400 + gap 150); plus 3000 margin, 6000 collect grace and 2500
     * per node. */
    const uint32_t window_ms = 300U + ((uint32_t)policy->samples_per_pair + policy->max_attempts - 1U) *
        ((uint32_t)policy->pair_timeout_ms + 150U);
    assert(window_ms == 25800U);
    for (unsigned nodes = 2U; nodes <= 5U; ++nodes) {
        const uint32_t budget_ms = nodes * (nodes - 1U) * window_ms + 3000U + 6000U + nodes * 2500U;
        assert(budget_ms < 600000U);
        if (nodes == 5U) assert(budget_ms == 537500U);               /* 9 minutes of silent bus */
        if (nodes == 4U) assert(budget_ms == 328600U);
    }
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

esp_err_t nrf_topology_service_ranging_cancel(void)
{
    return ESP_OK;
}

esp_err_t nrf_topology_service_ranging_set_manual(uint64_t started, uint64_t updated,
    uint8_t a, uint8_t b, int32_t distance, const uint32_t *limits)
{
    (void)started; (void)updated; (void)a; (void)b; (void)distance; (void)limits;
    return ESP_ERR_NOT_SUPPORTED;
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

void main_barometer_request_polling(void)
{
    ++s_barometer_polling_requests;
}

esp_err_t main_barometer_request_height_reference_capture(uint16_t generation)
{
    return generation != 0U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t s_device_snapshot_result = ESP_OK;

esp_err_t device_status_try_get_snapshot(device_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_device_snapshot_result != ESP_OK) return s_device_snapshot_result;
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
    workflow_host_rtos_reset_ticks();
    workflow_service_host_fakes_reset();
    s_next_poll_work_ticks = 0U;
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
    s_height_request_calls = 0U;
    s_barometer_polling_requests = 0U;
    s_imu_request_calls = 0U;
    s_cs_request_calls = 0U;
    s_imu_request_mask = 0U;
    s_cs_cycle_valid = false;
    memset(&s_cs_cycle, 0, sizeof(s_cs_cycle));
    s_now_ms = 1000U;
    s_placement_ready = false;
    s_device_snapshot_result = ESP_OK;
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

static workflow_snapshot_t try_snapshot(void);

static workflow_command_result_t command_with_params(
    workflow_command_t value,
    const workflow_command_params_t *params)
{
    assert(params != NULL);
    return workflow_service_command(value, params);
}

static void enter_stable_placement(void)
{
    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALIBRATION_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_PLACE_SENSORS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_placement_ready = true;
    s_device.main.bmp280_valid = true;
    s_device.main.pressure_pa = 101325U;
    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device.main.height_reference.generation = s_height_generation;
    for (unsigned sample = 0U; sample < WORKFLOW_PLACEMENT_MIN_SAMPLES; ++sample) {
        s_now_ms += 2000U;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (s_topology.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
            nrf_remote_sensor_cache_t *cache = &s_cache[channel];
            cache->identity_valid = true;
            cache->topology_current = true;
            cache->response_valid = true;
            cache->identity.node_id = s_topology.remotes[channel].node_id;
            cache->esp_receipt_uptime_ms = s_now_ms;
            cache->sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
            cache->sensor.pressure_pa = 101325U;
            cache->sensor.altitude_mm = (int32_t)(12000U + channel * 1000U);
            cache->sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
            cache->sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
            cache->sensor.reference_generation = s_height_generation;
            cache->sensor.relative_z_mm = (int32_t)(100U + channel * 100U);
        }
        assert(workflow_service_poll() == ESP_OK);
    }
    const workflow_snapshot_t placement = try_snapshot();
    assert(placement.step == WORKFLOW_STEP_PLACEMENT);
    assert(placement.placement.all_stable);
}

static void enter_gps_choice(void)
{
    enter_stable_placement();
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(try_snapshot().step == WORKFLOW_STEP_GPS_CHOICE);
}

static void enter_scan_choice(void)
{
    enter_gps_choice();
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_RANGING_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CONFIRM, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(try_snapshot().step == WORKFLOW_STEP_SCAN_CHOICE);
}

static void test_phone_gps_validates_and_persists_only_accepted_fixes(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    enter_gps_choice();
    workflow_snapshot_t snapshot = try_snapshot();
    const workflow_command_params_t request = {
        .has_generation = true,
        .expected_generation = snapshot.generation,
    };
    assert(command_with_params(WORKFLOW_COMMAND_REQUEST_GPS, &request) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_PENDING);

    workflow_command_params_t generationless_failure = {
        .reason = "phone_gps_unavailable",
    };
    assert(command_with_params(WORKFLOW_COMMAND_GPS_CAPTURE_FAILED, &generationless_failure) ==
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    assert(try_snapshot().step == WORKFLOW_STEP_GPS_PENDING);

    workflow_command_params_t gps = {
        .has_generation = true,
        .expected_generation = snapshot.generation,
        .has_location = true,
        .latitude = -90.0,
        .longitude = 180.0,
        .accuracy_m = 0.0,
        .altitude_valid = true,
        .altitude_m = 34.5,
        .altitude_accuracy_m = 0.5,
        .captured_at_ms = 123456U,
    };
    gps.has_generation = false;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) ==
        WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    assert(workflow_test_location_save_calls == 0U);
    assert(try_snapshot().step == WORKFLOW_STEP_GPS_PENDING);
    gps.has_generation = true;
    gps.latitude = 90.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.latitude = -90.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.latitude = -90.0;
    gps.longitude = 180.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.longitude = -180.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.longitude = 180.0;
    gps.accuracy_m = -0.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.accuracy_m = NAN;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.accuracy_m = INFINITY;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.accuracy_m = 0.0;
    gps.altitude_accuracy_m = -0.001;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.altitude_accuracy_m = NAN;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_INVALID_ARGUMENT);
    gps.altitude_accuracy_m = 0.5;
    assert(workflow_test_location_save_calls == 0U);
    assert(try_snapshot().step == WORKFLOW_STEP_GPS_PENDING);

    gps.latitude = 90.0;
    gps.longitude = -180.0;
    workflow_test_location_save_result = ESP_FAIL;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_PENDING);
    assert(workflow_test_location_save_calls == 1U);
    workflow_test_location_save_result = ESP_OK;
    gps.latitude = -90.0;
    gps.longitude = 180.0;
    gps.expected_generation = snapshot.generation;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &gps) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(snapshot.gps_status == WORKFLOW_GPS_STATUS_ACCEPTED);
    assert(workflow_test_location_save_calls == 2U);
    assert(strcmp(workflow_test_saved_location_object_id, "object-a") == 0);
    assert(workflow_test_saved_location.valid);
    assert(workflow_test_saved_location.latitude == -90.0);
    assert(workflow_test_saved_location.longitude == 180.0);
    assert(workflow_test_saved_location.accuracy_m == 0.0f);
    assert(workflow_test_saved_location.altitude_valid);
    assert(workflow_test_saved_location.captured_at_ms == 123456U);
    assert(strcmp(workflow_test_saved_location.source, "PHONE_GPS") == 0);

    workflow_service_shutdown();
}

static void test_gps_failure_back_skip_and_late_callback_generation(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_saved_location = (object_store_location_t){
        .valid = true,
        .latitude = 48.25,
        .longitude = 29.75,
        .accuracy_m = 17.0f,
        .captured_at_ms = 987654U,
    };
    (void)snprintf(workflow_test_saved_location.source,
        sizeof(workflow_test_saved_location.source), "%s", "MANUAL");
    const object_store_location_t stored_before_skip = workflow_test_saved_location;
    enter_gps_choice();
    workflow_snapshot_t snapshot = try_snapshot();
    workflow_command_params_t params = {
        .has_generation = true,
        .expected_generation = snapshot.generation,
    };
    assert(command_with_params(WORKFLOW_COMMAND_REQUEST_GPS, &params) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    const uint32_t pending_generation = snapshot.generation;
    params.expected_generation = pending_generation;
    params.reason = "phone_gps_unavailable";
    assert(command_with_params(WORKFLOW_COMMAND_GPS_CAPTURE_FAILED, &params) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_CHOICE);
    assert(snapshot.gps_status == WORKFLOW_GPS_STATUS_FAILED);

    params.reason = NULL;
    params.expected_generation = snapshot.generation;
    assert(command_with_params(WORKFLOW_COMMAND_REQUEST_GPS, &params) == WORKFLOW_COMMAND_RESULT_OK);
    const workflow_snapshot_t second_pending = try_snapshot();
    params.expected_generation = second_pending.generation;
    assert(command_with_params(WORKFLOW_COMMAND_BACK, &params) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_GPS_CHOICE);
    params.has_location = true;
    params.latitude = 49.0;
    params.longitude = 32.0;
    params.accuracy_m = 5.0;
    params.expected_generation = second_pending.generation;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_ACCEPT, &params) == WORKFLOW_COMMAND_RESULT_STALE);
    assert(workflow_test_location_save_calls == 0U);

    params.expected_generation = snapshot.generation;
    assert(command_with_params(WORKFLOW_COMMAND_GPS_SKIP, &params) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(snapshot.gps_status == WORKFLOW_GPS_STATUS_SKIPPED);
    assert(workflow_test_location_save_calls == 0U);
    assert(memcmp(&workflow_test_saved_location, &stored_before_skip,
        sizeof(stored_before_skip)) == 0);
    workflow_service_shutdown();
}

static void test_gps_skip_keeps_existing_location_and_marks_run_skipped(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_saved_location = (object_store_location_t){
        .valid = true,
        .latitude = 48.25,
        .longitude = 29.75,
        .accuracy_m = 17.0f,
        .captured_at_ms = 987654U,
    };
    (void)snprintf(workflow_test_saved_location.source,
        sizeof(workflow_test_saved_location.source), "%s", "MANUAL");
    const object_store_location_t stored_before_skip = workflow_test_saved_location;

    enter_gps_choice();
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    const workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_LAYOUT_READY);
    assert(snapshot.gps_status == WORKFLOW_GPS_STATUS_SKIPPED);
    assert(workflow_test_location_save_calls == 0U);
    assert(memcmp(&workflow_test_saved_location, &stored_before_skip,
        sizeof(stored_before_skip)) == 0);
    workflow_service_shutdown();
}

static void test_scan_choice_requires_explicit_skip_before_recording(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    enter_scan_choice();
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_SCAN_CHOICE);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_SCAN_SKIP)) != 0U);
    assert((snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_START_RECORDING)) == 0U);
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(try_snapshot().step == WORKFLOW_STEP_SCAN_CHOICE);
    assert(command(WORKFLOW_COMMAND_SCAN_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RECORD_READY);
    assert(snapshot.scan_choice == WORKFLOW_SCAN_CHOICE_NO);
    workflow_service_shutdown();
}

static void enter_record_ready(void);

/* MAX-491 mutation-only: illegal and repeated recording commands have one side effect each. */
static void test_service_fsm_illegal_events_and_single_recording_lifecycle(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_OBJECT);
    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    workflow_snapshot_t after = try_snapshot();
    assert(memcmp(&after, &snapshot, sizeof(snapshot)) == 0);
    assert(s_start_calls == 0U && s_stop_calls == 0U);

    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    enter_record_ready();
    assert(try_snapshot().step == WORKFLOW_STEP_RECORD_READY);
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_start_calls == 1U);
    assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(s_start_calls == 1U);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_RECORDING);
    assert(strcmp(snapshot.session_id, "session-1") == 0);

    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_stop_calls == 1U);
    s_measurement.state = MEASUREMENT_SERVICE_REVIEW;
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_STOPPED);
    assert(strcmp(snapshot.session_id, "session-1") == 0);
    assert(command(WORKFLOW_COMMAND_STOP_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(s_stop_calls == 1U);
    workflow_service_shutdown();
}

static void wait_workflow_without_command(workflow_step_t expected)
{
    workflow_snapshot_t before = try_snapshot();
    assert(before.step == expected);
    for (unsigned cycle = 0U; cycle < 3U; ++cycle) {
        s_now_ms += UINT64_C(60000);
        assert(workflow_service_poll() == ESP_OK);
        const workflow_snapshot_t after = try_snapshot();
        assert(after.step == expected);
        assert(after.generation == before.generation);
    }
}

static void refresh_placement_gate(void)
{
    for (unsigned sample = 0U; sample < WORKFLOW_PLACEMENT_MIN_SAMPLES; ++sample) {
        s_now_ms += 2000U;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (s_topology.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
                s_cache[channel].esp_receipt_uptime_ms = s_now_ms;
            }
        }
        assert(workflow_service_poll() == ESP_OK);
    }
}

static void test_new_workflow_steps_wait_for_explicit_commands(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    enter_gps_choice();
    wait_workflow_without_command(WORKFLOW_STEP_GPS_CHOICE);
    assert(command(WORKFLOW_COMMAND_REQUEST_GPS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    wait_workflow_without_command(WORKFLOW_STEP_GPS_PENDING);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    wait_workflow_without_command(WORKFLOW_STEP_LAYOUT_READY);
    refresh_placement_gate();
    assert(try_snapshot().placement.all_stable);

    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_ranging_progress.complete = true;
    s_ranging_progress.success = true;
    assert(workflow_service_poll() == ESP_OK);
    assert(command(WORKFLOW_COMMAND_CONFIRM, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    wait_workflow_without_command(WORKFLOW_STEP_SCAN_CHOICE);
    workflow_test_staged_scan_present = true;
    assert(command(WORKFLOW_COMMAND_SCAN_START, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    wait_workflow_without_command(WORKFLOW_STEP_SCANNING);
    workflow_service_shutdown();
}

static void prepare_max460_wait_step(workflow_step_t step)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    workflow_test_saved_location = (object_store_location_t){
        .valid = true,
        .latitude = 48.25,
        .longitude = 29.75,
        .accuracy_m = 17.0f,
        .captured_at_ms = 987654U,
    };
    (void)snprintf(workflow_test_saved_location.source,
        sizeof(workflow_test_saved_location.source), "%s", "MANUAL");
    switch (step) {
        case WORKFLOW_STEP_PLACEMENT:
            enter_stable_placement();
            break;
        case WORKFLOW_STEP_GPS_CHOICE:
            enter_gps_choice();
            break;
        case WORKFLOW_STEP_GPS_PENDING:
            enter_gps_choice();
            workflow_test_saved_location = (object_store_location_t){
                .valid = true,
                .latitude = 48.25,
                .longitude = 29.75,
                .accuracy_m = 17.0f,
            };
            assert(command(WORKFLOW_COMMAND_REQUEST_GPS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
            break;
        case WORKFLOW_STEP_LAYOUT_READY:
            enter_gps_choice();
            assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
            break;
        case WORKFLOW_STEP_SCAN_CHOICE:
            enter_scan_choice();
            workflow_test_staged_scan_present = true;
            break;
        case WORKFLOW_STEP_SCANNING:
            enter_scan_choice();
            workflow_test_staged_scan_present = true;
            assert(command(WORKFLOW_COMMAND_SCAN_START, false, false) == WORKFLOW_COMMAND_RESULT_OK);
            break;
        default:
            assert(false);
            break;
    }
}

static void poll_after_fake_time_advance(
    workflow_step_t expected_step,
    uint32_t expected_generation,
    uint64_t advance_ms)
{
    s_now_ms += advance_ms;
    for (unsigned sample = 0U; sample < WORKFLOW_PLACEMENT_MIN_SAMPLES; ++sample) {
        s_now_ms += 2000U;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (s_topology.remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED) {
                s_cache[channel].esp_receipt_uptime_ms = s_now_ms;
            }
        }
        assert(workflow_service_poll() == ESP_OK);
        const workflow_snapshot_t after_poll = try_snapshot();
        assert(after_poll.step == expected_step);
        assert(after_poll.generation == expected_generation);
    }
}

static void test_no_step_advances_without_a_command_fake_clock(void)
{
    static const struct {
        workflow_step_t step;
        workflow_command_t explicit_command;
        workflow_step_t next_step;
    } cases[] = {
        { WORKFLOW_STEP_PLACEMENT, WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, WORKFLOW_STEP_GPS_CHOICE },
        { WORKFLOW_STEP_GPS_CHOICE, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
        { WORKFLOW_STEP_GPS_PENDING, WORKFLOW_COMMAND_GPS_SKIP, WORKFLOW_STEP_LAYOUT_READY },
        { WORKFLOW_STEP_LAYOUT_READY, WORKFLOW_COMMAND_CALCULATE_POSITIONS, WORKFLOW_STEP_RANGING },
        { WORKFLOW_STEP_SCAN_CHOICE, WORKFLOW_COMMAND_SCAN_SKIP, WORKFLOW_STEP_RECORD_READY },
        { WORKFLOW_STEP_SCANNING, WORKFLOW_COMMAND_BACK, WORKFLOW_STEP_SCAN_CHOICE },
    };
    static const uint64_t waits_ms[] = { UINT64_C(1000), UINT64_C(60000), UINT64_C(3600000) };

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        prepare_max460_wait_step(cases[index].step);
        workflow_snapshot_t before = try_snapshot();
        assert(before.step == cases[index].step);
        if (cases[index].step == WORKFLOW_STEP_PLACEMENT ||
            cases[index].step == WORKFLOW_STEP_GPS_CHOICE ||
            cases[index].step == WORKFLOW_STEP_GPS_PENDING ||
            cases[index].step == WORKFLOW_STEP_LAYOUT_READY) {
            assert(before.placement.all_stable);
        }

        for (size_t wait = 0U; wait < sizeof(waits_ms) / sizeof(waits_ms[0]); ++wait) {
            poll_after_fake_time_advance(cases[index].step, before.generation, waits_ms[wait]);
        }

        assert(command(cases[index].explicit_command, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        const workflow_snapshot_t after_command = try_snapshot();
        assert(after_command.step == cases[index].next_step);
        assert(after_command.generation != before.generation);
        workflow_service_shutdown();
    }
}

static void test_staged_scan_remains_visible_across_session_creation(void)
{
    for (unsigned promotion_succeeded = 0U; promotion_succeeded < 2U; ++promotion_succeeded) {
        reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
        enter_scan_choice();
        workflow_test_staged_scan_present = true;
        assert(command(WORKFLOW_COMMAND_SCAN_START, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_SCAN_COMPLETE, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        workflow_snapshot_t completed = try_snapshot();
        assert(completed.step == WORKFLOW_STEP_RECORD_READY);
        assert(completed.scan_choice == WORKFLOW_SCAN_CHOICE_YES);
        assert(completed.scan_upload_status == WORKFLOW_SCAN_UPLOAD_VERIFIED);
        assert(completed.has_scan);
        assert(command(WORKFLOW_COMMAND_START_RECORDING, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        workflow_test_session_scan_present = promotion_succeeded != 0U;
        workflow_test_staged_scan_present = promotion_succeeded == 0U;
        assert(workflow_service_poll() == ESP_OK);
        const workflow_snapshot_t snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_RECORDING);
        assert(strcmp(snapshot.session_id, "session-1") == 0);
        assert(snapshot.has_scan);
        workflow_service_shutdown();
    }
}

static void enter_record_ready(void)
{
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALIBRATION_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_PLACE_SENSORS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_placement_ready = true;
    workflow_snapshot_t placement_snapshot;
    /* One reading per refresh period (2 s); the window needs WORKFLOW_PLACEMENT_MIN_SAMPLES of them. */
    for (unsigned sample = 0U; sample < WORKFLOW_PLACEMENT_MIN_SAMPLES; ++sample) {
        s_now_ms += 2000U;
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
        assert(workflow_service_get_snapshot(&placement_snapshot) == ESP_OK);
        if (sample + 1U < WORKFLOW_PLACEMENT_MIN_SAMPLES) {
            assert(!placement_snapshot.placement.all_stable);
            assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) ==
                WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
        }
    }
    assert(placement_snapshot.placement.all_stable);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_RANGING_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CONFIRM, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SCAN_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
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

static void test_sensor_check_requests_barometer_before_calibration(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 1U);
    assert(workflow_service_poll() == ESP_OK);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_OBJECT);
    assert(s_barometer_polling_requests == 0U);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) ==
        WORKFLOW_COMMAND_RESULT_NOT_ALLOWED);
    assert(s_barometer_polling_requests == 0U);

    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(s_barometer_polling_requests == 1U);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
    assert(workflow_service_poll() == ESP_OK);
    assert(s_barometer_polling_requests == 1U);
    assert(s_height_request_calls == 0U && s_imu_request_calls == 0U);
    assert(s_start_calls == 0U);

    assert(command(WORKFLOW_COMMAND_BACK, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_barometer_polling_requests == 1U);
    uint32_t seq = 0U;
    assert(workflow_service_command_async(WORKFLOW_COMMAND_START_MEASUREMENT,
        false, 0U, &seq) == ESP_OK);
    assert(s_barometer_polling_requests == 1U);
    assert(workflow_service_poll() == ESP_OK);
    assert(s_barometer_polling_requests == 2U);
    snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_SENSOR_CHECK);
    assert(snapshot.last_async.seq == seq);
    assert(snapshot.last_async.result == WORKFLOW_COMMAND_RESULT_OK);

    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) ==
        WORKFLOW_COMMAND_RESULT_OK);
    assert(s_barometer_polling_requests == 2U);
    assert(command(WORKFLOW_COMMAND_BACK, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_barometer_polling_requests == 3U);
    assert(s_height_request_calls == 0U && s_start_calls == 0U);
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
extern uint32_t workflow_service_test_poll_missed_deadline_count(void);

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

    const cJSON *gps_status = cJSON_GetObjectItemCaseSensitive(workflow, "gps_status");
    assert(cJSON_IsString(gps_status));
    assert(strcmp(gps_status->valuestring, workflow_core_gps_status_name(expected->gps_status)) == 0);
    const cJSON *scan_choice = cJSON_GetObjectItemCaseSensitive(workflow, "scan_choice");
    assert(cJSON_IsString(scan_choice));
    assert(strcmp(scan_choice->valuestring, workflow_core_scan_choice_name(expected->scan_choice)) == 0);
    const cJSON *scan_upload_status = cJSON_GetObjectItemCaseSensitive(workflow, "scan_upload_status");
    assert(cJSON_IsString(scan_upload_status));
    assert(strcmp(scan_upload_status->valuestring,
        workflow_core_scan_upload_status_name(expected->scan_upload_status)) == 0);
    const cJSON *layout_result = cJSON_GetObjectItemCaseSensitive(workflow, "layout_result");
    if (expected->layout_result_valid) {
        assert(cJSON_IsBool(layout_result));
        assert(cJSON_IsTrue(layout_result) == expected->layout_ok);
    } else {
        assert(cJSON_IsNull(layout_result));
    }
    const cJSON *layout_reason = cJSON_GetObjectItemCaseSensitive(workflow, "layout_reason");
    assert(cJSON_IsString(layout_reason));
    assert(strcmp(layout_reason->valuestring, expected->reason) == 0);

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
        if (i == 2U) {
            workflow_service_queue_stats_t queue_stats;
            assert(workflow_service_get_queue_stats(&queue_stats) == ESP_OK);
            assert(queue_stats.peak_occupancy == 3U && queue_stats.overflow_count == 0U);
        }
    }
    uint32_t unchanged = UINT32_C(0x12345678);
    assert(workflow_service_command_async(WORKFLOW_COMMAND_BACK,
        false, 0U, &unchanged) == ESP_ERR_NO_MEM);
    assert(unchanged == UINT32_C(0x12345678));
    workflow_service_queue_stats_t queue_stats;
    assert(workflow_service_get_queue_stats(&queue_stats) == ESP_OK);
    assert(queue_stats.peak_occupancy == 4U && queue_stats.overflow_count == 1U);
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(snapshot.last_async.seq == accepted[3]);
    uint32_t next_accepted = 0U;
    assert(workflow_service_command_async(
        WORKFLOW_COMMAND_BACK, false, 0U, &next_accepted
    ) == ESP_OK);
    uint32_t expected_next = accepted[3] + 1U;
    if (expected_next == 0U) expected_next = 1U;
    assert(next_accepted == expected_next);
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
        assert(snapshot.calibration.pairs_total == 0U && s_cs_request_calls == 0U);
        s_now_ms += UINT64_C(15000);
        assert(workflow_service_poll() == ESP_OK);
        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
        assert(snapshot.calibration.result_valid && !snapshot.calibration.result_ok);
        assert(snapshot_has_calibration_reason(&snapshot, "timeout"));
        workflow_service_shutdown();
    }
}


static void test_one_and_two_connected_remotes_complete_initialization(void)
{
    const uint8_t channels[] = {3U, 1U};
    for (size_t remotes = 1U; remotes <= 2U; ++remotes) {
        reset_fakes(true, MEASUREMENT_SERVICE_LIVE, remotes);
        memset(&s_topology, 0, sizeof(s_topology));
        uint8_t expected_mask = 0U;
        for (size_t i = 0U; i < remotes; ++i) {
            const uint8_t channel = channels[i];
            s_topology.remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
            s_topology.remotes[channel].node_id = (uint8_t)(channel + 1U);
            expected_mask |= (uint8_t)(1U << channel);
        }
        assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        /* Calibration is height + IMU only: no Channel Sounding (BLE) traffic before ranging. */
        assert(s_height_request_calls == 1U && s_imu_request_calls == 1U && s_cs_request_calls == 0U);
        assert(s_imu_request_mask == expected_mask);
        workflow_snapshot_t snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_CALIBRATING);
        assert(snapshot.calibration.per_node_count == remotes + 1U);
        assert(snapshot.calibration.pairs_total == 0U);

        s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
        s_device.main.height_reference.generation = s_height_generation;
        /* No CS cycle exists at all; calibration must finish on height and IMU alone. */
        s_cs_cycle_valid = false;
        for (size_t i = 0U; i < remotes; ++i) {
            const uint8_t channel = channels[i];
            nrf_remote_sensor_cache_t *cache = &s_cache[channel];
            cache->identity_valid = true;
            cache->topology_current = true;
            cache->response_valid = true;
            cache->identity.node_id = (uint8_t)(channel + 1U);
            cache->sensor.reference_generation = s_height_generation;
            cache->sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
            cache->sensor.imu_cal_generation = s_height_generation;
            cache->sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
        }
        s_now_ms += 250U;
        assert(workflow_service_poll() == ESP_OK);
        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
        assert(snapshot.calibration.result_valid && snapshot.calibration.result_ok);
        assert(snapshot.calibration.pairs_done == 0U && snapshot.calibration.reasons_count == 0U);
        assert(s_cs_request_calls == 0U);
        workflow_service_shutdown();
    }
}

static void test_rejected_imu_calibration_command_is_repeated_for_silent_nodes_only(void)
{
    /* 2026-10-31 15:31 log: G1 stayed imu=pending until the 15 s timeout while G2 and G4 calibrated. The nRF
     * answers a command it cannot take (session busy) only in its own TWIS log; the I2C write is still
     * ACKed, so the ESP sent the command once and never learned that it was dropped. */
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 2U);
    memset(&s_topology, 0, sizeof(s_topology));
    const uint8_t channels[] = {3U, 1U};
    for (size_t i = 0U; i < 2U; ++i) {
        s_topology.remotes[channels[i]].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        s_topology.remotes[channels[i]].node_id = (uint8_t)(channels[i] + 1U);
    }
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(s_imu_request_calls == 1U);
    assert(s_imu_request_mask == ((1U << 3U) | (1U << 1U)));

    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device.main.height_reference.generation = s_height_generation;
    for (size_t i = 0U; i < 2U; ++i) {
        nrf_remote_sensor_cache_t *cache = &s_cache[channels[i]];
        cache->identity_valid = true;
        cache->topology_current = true;
        cache->response_valid = true;
        cache->identity.node_id = (uint8_t)(channels[i] + 1U);
        cache->sensor.reference_generation = s_height_generation;
        cache->sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
    }
    /* Channel 3 calibrates; channel 1 (G2) never answers: the command was dropped. */
    s_cache[3].sensor.imu_cal_generation = s_height_generation;
    s_cache[3].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;

    s_now_ms += 1000U;
    assert(workflow_service_poll() == ESP_OK);
    assert(s_imu_request_calls == 1U); /* too early for a repeat */

    s_now_ms += 2500U;
    assert(workflow_service_poll() == ESP_OK);
    assert(s_imu_request_calls == 2U);
    assert(s_imu_request_mask == (1U << 1U)); /* only the silent node, never the one that answered */
    assert(s_imu_request_generation == s_height_generation);

    s_now_ms += 3100U;
    assert(workflow_service_poll() == ESP_OK);
    assert(s_imu_request_calls == 3U);
    assert(s_imu_request_mask == (1U << 1U));

    /* The node finally answers: the repeats stop and the run completes. */
    s_cache[1].sensor.imu_cal_generation = s_height_generation;
    s_cache[1].sensor.imu_cal_state = NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED;
    s_now_ms += 250U;
    assert(workflow_service_poll() == ESP_OK);
    s_now_ms += 5000U;
    assert(workflow_service_poll() == ESP_OK);
    assert(s_imu_request_calls == 3U);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.step == WORKFLOW_STEP_CALIB_RESULT);
    assert(snapshot.calibration.result_valid && snapshot.calibration.result_ok);
    workflow_service_shutdown();
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
            s_now_ms += 2000U;      /* the production refresh period: readings closer than 1 s are not counted */
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
        assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
        assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);

        snapshot = try_snapshot();
        assert(snapshot.step == WORKFLOW_STEP_RANGING_RESULT);
        assert(strcmp(snapshot.reason, "insufficient_nodes") == 0);
        assert(snapshot.allowed_actions & WORKFLOW_ACTION(WORKFLOW_COMMAND_RETRY));
        workflow_service_shutdown();
    }
}

/* Deterministic reading noise (uniform, +-amplitude) for the tests that need a barometer that is not perfect. */
static int32_t s_feed_noise_amplitude_mm;
static uint32_t s_feed_noise_state = 12345U;

static int32_t feed_noise_mm(void)
{
    if (s_feed_noise_amplitude_mm == 0) return 0;
    s_feed_noise_state = s_feed_noise_state * 1664525U + 1013904223U;
    const int32_t span = 2 * s_feed_noise_amplitude_mm + 1;
    return (int32_t)((s_feed_noise_state >> 8) % (uint32_t)span) - s_feed_noise_amplitude_mm;
}

/* One sensor refresh answered by every assigned remote; channel 1 (G2) can be offset in height. */
static void feed_remote_sensor_refresh(int32_t channel1_offset_mm)
{
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (s_topology.remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        nrf_remote_sensor_cache_t *cache = &s_cache[channel];
        cache->identity_valid = true;
        cache->topology_current = true;
        cache->response_valid = true;
        cache->identity.node_id = s_topology.remotes[channel].node_id;
        cache->esp_receipt_uptime_ms = s_now_ms;
        cache->sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_ENV;
        cache->sensor.pressure_pa = 101325U;
        cache->sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
        cache->sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
        cache->sensor.reference_generation = s_height_generation;
        cache->sensor.relative_z_mm = (int32_t)(100U + channel * 100U) +
            (channel == 1U ? channel1_offset_mm : 0) + feed_noise_mm();
    }
}

static void begin_placement_without_samples(size_t remotes)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, remotes);
    assert(workflow_service_set_placement_ready_predicate(placement_ready, NULL) == ESP_OK);
    assert(command(WORKFLOW_COMMAND_START_MEASUREMENT, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GO_CALIBRATION, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_SENSORS_PLACED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALIBRATION_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_PLACE_SENSORS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_placement_ready = true;
    s_device.main.bmp280_valid = true;
    s_device.main.pressure_pa = 101325U;
    s_device.main.height_reference.state = DEVICE_MAIN_HEIGHT_REFERENCE_VALID;
    s_device.main.height_reference.generation = s_height_generation;
}

/* Advances 100 ms polls; the remotes answer a sensor refresh every 2 s counted from `refresh_origin_ms`. */
static void run_placement_polls(unsigned duration_ms, uint64_t refresh_origin_ms, int32_t channel1_offset_mm)
{
    for (unsigned elapsed = 0U; elapsed < duration_ms; elapsed += 100U) {
        s_now_ms += 100U;
        if (((s_now_ms - refresh_origin_ms) % 2000U) == 0U) feed_remote_sensor_refresh(channel1_offset_mm);
        assert(workflow_service_poll() == ESP_OK);
    }
}

static const workflow_placement_node_t *placement_node(const workflow_snapshot_t *snapshot, const char *name)
{
    for (size_t i = 0U; i < snapshot->placement.per_node_count; ++i) {
        if (strcmp(snapshot->placement.per_node[i].node, name) == 0) return &snapshot->placement.per_node[i];
    }
    assert(!"placement node not found");
    return NULL;
}

static unsigned count_log_matches(const char *needle)
{
    unsigned matches = 0U;
    for (const char *at = workflow_test_log_buffer; (at = strstr(at, needle)) != NULL; at += strlen(needle)) ++matches;
    return matches;
}

/* Until 2026-11-01 21:46 the log said only "RANGING_AUTONOMOUS result=0": a failed positions step was invisible. */
static void test_ranging_result_is_logged_with_every_pair_and_node(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    enter_gps_choice();
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    refresh_placement_gate();
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_test_log_clear();

    s_ranging_progress.complete = true;
    s_ranging_progress.success = false;
    s_ranging_progress.partial = true;
    (void)snprintf(s_ranging_progress.failure_reason, sizeof(s_ranging_progress.failure_reason), "pair_failed:0-2");
    s_ranging_progress.pairs_done = 10U;
    s_ranging_progress.pairs_total = 12U;
    s_ranging_progress.pair_count = 2U;
    s_ranging_progress.pairs[0] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 1U, .state = NRF_CS_PAIR_VALID, .distance_mm = 2345, .confidence = 900U,
        .attempts = 7U, .valid_samples = 5U, .spread_mm = 12, .rejected_samples = 1U, .automatic_distance_mm = 2340,
        .direction_disagreement_mm = 3152, .single_direction = false,
    };
    s_ranging_progress.pairs[1] = (nrf_cs_pair_result_t){
        .node_a = 0U, .node_b = 2U, .state = NRF_CS_PAIR_MISSING, .distance_mm = -1, .attempts = 3U,
    };
    s_ranging_progress.solution_residual_mm = 74;
    s_ranging_progress.geometry.max_residual_mm = 74;
    s_ranging_progress.geometry.inconsistent_pair_count = 1U;
    s_ranging_progress.geometry.node_count = 2U;
    s_ranging_progress.geometry.nodes[0] = (nrf_topology_ranging_node_t){
        .node_id = 0U, .solved = true, .x_mm = 0, .y_mm = 0, .z_mm = 0, .residual_mm = 0, .uncertainty_mm = 5,
    };
    s_ranging_progress.geometry.nodes[1] = (nrf_topology_ranging_node_t){
        .node_id = 1U, .solved = true, .x_mm = 2345, .y_mm = -120, .z_mm = -180, .residual_mm = 74, .uncertainty_mm = 90,
    };
    s_ranging_progress.main_height_reference_generation = 9U;
    assert(workflow_service_poll() == ESP_OK);

    assert(count_log_matches("RANGING_RESULT success=0 partial=1 reason=pair_failed:0-2 executions=10/12 pairs=2 "
        "residual_mm=74 max_residual_mm=74 inconsistent_pairs=1 main_reference_generation=9") == 1U);
    assert(count_log_matches("RANGING_PAIR 0-1 state=valid distance_mm=2345 confidence=900 valid_samples=5 attempts=7 "
        "rejected=1 spread_mm=12 automatic_mm=2340 disagreement_mm=3152 single_direction=0") == 1U);
    assert(count_log_matches("RANGING_PAIR 0-2 state=missing distance_mm=-1") == 1U);
    assert(count_log_matches("RANGING_NODE id=1 solved=1 x_mm=2345 y_mm=-120 z_mm=-180 residual_mm=74 "
        "uncertainty_mm=90") == 1U);
    assert(count_log_matches("RANGING_NODE id=0 solved=1") == 1U);
    /* Logged once per ranging, not on every poll. */
    assert(workflow_service_poll() == ESP_OK);
    assert(count_log_matches("RANGING_RESULT ") == 1U);
    workflow_service_shutdown();
}

static void test_placement_becomes_stable_at_the_production_remote_refresh_cadence(void)
{
    /*
     * REMOTE sensor data reaches the ESP once per NRF_SENSOR_REFRESH_PERIOD_MS (2 s), MAIN every poll. The
     * stability check needs WORKFLOW_PLACEMENT_MIN_SAMPLES samples inside its window, so a window shorter
     * than (MIN_SAMPLES - 1) refresh periods can never be satisfied: 2026-10-31 16:24 bench, the stability
     * screen never let the user continue. The eighth reading arrives 16 s after the first.
     */
    begin_placement_without_samples(3U);
    uint64_t first_stable_ms = 0U;
    unsigned flaps = 0U;
    const uint64_t start_ms = s_now_ms;
    for (unsigned step = 0U; step < 450U; ++step) {          /* 45 s in 100 ms polls */
        run_placement_polls(100U, start_ms, 0);
        const workflow_snapshot_t snapshot = try_snapshot();
        if (snapshot.placement.all_stable) {
            if (first_stable_ms == 0U) first_stable_ms = s_now_ms - start_ms;
        } else if (first_stable_ms != 0U) {
            ++flaps;
        }
    }
    assert(first_stable_ms != 0U);
    assert(first_stable_ms >= (WORKFLOW_PLACEMENT_MIN_SAMPLES - 1U) * 2000U);  /* never before the 8th reading */
    assert(first_stable_ms <= WORKFLOW_PLACEMENT_MIN_SAMPLES * 2000U + 1000U);
    /* Once reached it must hold between refreshes instead of dropping out before each one. */
    assert(flaps == 0U);
    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    workflow_service_shutdown();
}

static void test_placement_with_noisy_barometers_settles_and_reports_the_true_height(void)
{
    /*
     * The bench of 2026-11-01 showed 70..130 mm of standard deviation per node. Uniform noise of +-150 mm
     * (sigma 87 mm) is the same order. The gate must neither stay shut for ever (the old 50 mm spread rule did)
     * nor open on a window that cannot vouch for 0.1 m, and the height it reports must be the true one.
     */
    begin_placement_without_samples(3U);
    s_feed_noise_amplitude_mm = 150;
    const uint64_t start_ms = s_now_ms;
    bool was_stable = false;
    unsigned flaps = 0U;
    for (unsigned step = 0U; step < 900U; ++step) {          /* 90 s in 100 ms polls */
        run_placement_polls(100U, start_ms, 0);
        const workflow_snapshot_t snapshot = try_snapshot();
        if (snapshot.placement.all_stable) was_stable = true;
        else if (was_stable) ++flaps;
    }
    const workflow_snapshot_t snapshot = try_snapshot();
    s_feed_noise_amplitude_mm = 0;
    assert(was_stable);
    assert(flaps <= 3U);
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        char name[8];
        (void)snprintf(name, sizeof(name), "G%u", (unsigned)(channel + 1U));
        const workflow_placement_node_t *node = placement_node(&snapshot, name);
        const int32_t truth = (int32_t)(100U + channel * 100U);
        assert(node->sample_count >= WORKFLOW_PLACEMENT_MIN_SAMPLES);
        assert(node->height_rel_main_mm > truth - 100 && node->height_rel_main_mm < truth + 100);
        if (node->stable) assert(node->range_mm <= WORKFLOW_PLACEMENT_MAX_RANGE_MM);
    }
    workflow_service_shutdown();
}

static void test_placement_reports_why_each_node_is_not_stable_and_logs_the_readings(void)
{
    begin_placement_without_samples(3U);
    workflow_test_log_clear();
    const uint64_t origin_ms = s_now_ms;

    /* Before the first refresh the remotes have no current snapshot; then samples accumulate. */
    run_placement_polls(1000U, origin_ms, 0);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(placement_node(&snapshot, "G1")->reason == WORKFLOW_PLACEMENT_REASON_NODE_STALE);
    assert(placement_node(&snapshot, "MAIN")->reason == WORKFLOW_PLACEMENT_REASON_COLLECTING ||
           placement_node(&snapshot, "MAIN")->reason == WORKFLOW_PLACEMENT_REASON_STABLE);
    run_placement_polls(15000U, origin_ms, 0);
    snapshot = try_snapshot();
    assert(snapshot.placement.all_stable);
    assert(placement_node(&snapshot, "G1")->reason == WORKFLOW_PLACEMENT_REASON_STABLE);
    assert(placement_node(&snapshot, "G1")->sample_count >= WORKFLOW_PLACEMENT_MIN_SAMPLES);
    assert(placement_node(&snapshot, "G1")->range_mm == 0);
    run_placement_polls(2500U, origin_ms, 0);   /* long enough for one periodic log line */

    /* The readings are logged: each state change once, and a periodic line while the state holds. */
    assert(count_log_matches("PLACEMENT_STABILITY node=G1 stable=0 reason=node_stale trigger=change") == 1U);
    assert(count_log_matches("PLACEMENT_STABILITY node=G1 stable=0 reason=collecting trigger=change") == 1U);
    assert(count_log_matches("PLACEMENT_STABILITY node=G1 stable=1 reason=stable trigger=change") == 1U);
    assert(count_log_matches("PLACEMENT_STABILITY node=G1 stable=1 reason=stable trigger=periodic") >= 1U);
    assert(strstr(workflow_test_log_buffer, "limit_mm=100") != NULL);
    assert(strstr(workflow_test_log_buffer, "sigma_mm=0 u95_mm=0 shift_mm=0 outliers=0") != NULL);
    assert(strstr(workflow_test_log_buffer, "pressure_pa=101325") != NULL);
    assert(strstr(workflow_test_log_buffer, "age_ms=") != NULL);

    /* A node carried 500 mm higher (two readings so far) names the uncertainty of its mean: 8 x 200 and 2 x 700 mm. */
    workflow_test_log_clear();
    run_placement_polls(4000U, origin_ms, 500);
    snapshot = try_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(placement_node(&snapshot, "G2")->reason == WORKFLOW_PLACEMENT_REASON_RANGE_EXCEEDED);
    assert(placement_node(&snapshot, "G2")->range_mm == 134);
    assert(placement_node(&snapshot, "G2")->height_rel_main_mm == 300);
    assert(placement_node(&snapshot, "G1")->reason == WORKFLOW_PLACEMENT_REASON_STABLE);
    assert(count_log_matches("PLACEMENT_STABILITY node=G2 stable=0 reason=range_exceeded trigger=change") == 1U);
    assert(strstr(workflow_test_log_buffer, "range_mm=134") != NULL);
    assert(strstr(workflow_test_log_buffer, "height_mm=300") != NULL);
    assert(count_log_matches("node=G1 stable=1 reason=stable trigger=change") == 0U);

    /* A node that is not upright. */
    s_cache[0].sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED - 1U; /* moving */
    workflow_test_log_clear();
    run_placement_polls(100U, origin_ms, 0);
    snapshot = try_snapshot();
    assert(placement_node(&snapshot, "G1")->reason == WORKFLOW_PLACEMENT_REASON_NOT_VERTICAL);
    assert(!placement_node(&snapshot, "G1")->stable);
    assert(count_log_matches("PLACEMENT_STABILITY node=G1 stable=0 reason=not_vertical trigger=change") == 1U);

    /* MAIN's barometer gone: for a few seconds the collected samples are kept, then the cause is named. */
    s_device.main.bmp280_valid = false;
    workflow_test_log_clear();
    run_placement_polls(100U, origin_ms, 0);
    snapshot = try_snapshot();
    assert(placement_node(&snapshot, "MAIN")->reason != WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER);
    run_placement_polls(WORKFLOW_PLACEMENT_MAIN_BARO_GRACE_MS, origin_ms, 0);
    snapshot = try_snapshot();
    assert(placement_node(&snapshot, "MAIN")->reason == WORKFLOW_PLACEMENT_REASON_MAIN_BAROMETER);
    assert(count_log_matches("PLACEMENT_STABILITY node=MAIN stable=0 reason=main_barometer trigger=change") == 1U);
    s_device.main.bmp280_valid = true;

    /* The remotes stop answering: not current any more. */
    run_placement_polls(100U, origin_ms, 0);
    s_now_ms += 6000U;
    assert(workflow_service_poll() == ESP_OK);
    snapshot = try_snapshot();
    assert(placement_node(&snapshot, "G3")->reason == WORKFLOW_PLACEMENT_REASON_NODE_STALE);
    workflow_service_shutdown();
}

static void test_placement_survives_a_missed_device_status_read_but_not_a_long_outage(void)
{
    /* device_status_try_get_snapshot() never waits for its lock; a poll that lands on a writer got nothing and
     * used to wipe every node's history, so the stability window had to start over. */
    begin_placement_without_samples(3U);
    const uint64_t origin_ms = s_now_ms;
    run_placement_polls(16000U, origin_ms, 0);
    assert(try_snapshot().placement.all_stable);

    s_device_snapshot_result = ESP_ERR_TIMEOUT;
    run_placement_polls(100U, origin_ms, 0);
    workflow_snapshot_t snapshot = try_snapshot();
    assert(snapshot.placement.all_stable);
    assert(placement_node(&snapshot, "MAIN")->reason == WORKFLOW_PLACEMENT_REASON_STABLE);

    s_device_snapshot_result = ESP_OK;
    run_placement_polls(100U, origin_ms, 0);
    assert(try_snapshot().placement.all_stable);

    /* Without status for longer than the hold the value is not trusted: the cause is named. */
    s_device_snapshot_result = ESP_ERR_TIMEOUT;
    run_placement_polls(1500U, origin_ms, 0);
    snapshot = try_snapshot();
    assert(!snapshot.placement.all_stable);
    assert(placement_node(&snapshot, "MAIN")->reason == WORKFLOW_PLACEMENT_REASON_DEVICE_STATUS_BUSY);
    workflow_service_shutdown();
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

/*
 * Bench log 2026-11-01 21:46: workflow_poll read the SD card every ~2 s during the live preview; each read waited
 * 80..440 ms for the LVGL task (the card shares the display's SPI bus), held workflow_poll up and missed deadlines.
 * No step from the calibration to the ranging result uses what it reads.
 */
static void test_the_sd_card_is_left_alone_from_calibration_to_the_ranging_result(void)
{
    begin_placement_without_samples(3U);
    const uint64_t origin_ms = s_now_ms;
    run_placement_polls(2000U, origin_ms, 0);
    const unsigned reads_on_entry = workflow_test_scan_calls;

    run_placement_polls(30000U, origin_ms, 0);                      /* 30 s of polls on the placement */
    assert(workflow_test_scan_calls == reads_on_entry);
    assert(try_snapshot().placement.all_stable);

    assert(command(WORKFLOW_COMMAND_PLACEMENT_CONFIRMED, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_GPS_SKIP, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_CALCULATE_POSITIONS, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    assert(command(WORKFLOW_COMMAND_RANGING_DONE, true, true) == WORKFLOW_COMMAND_RESULT_OK);
    assert(try_snapshot().step == WORKFLOW_STEP_RANGING_RESULT);
    for (unsigned i = 0U; i < 10U; ++i) {
        s_now_ms += 2000U;
        assert(workflow_service_poll() == ESP_OK);
    }
    assert(workflow_test_scan_calls == reads_on_entry);             /* the live preview polls are quiet too */

    /* The scan choice is where the card is read again: the first poll on the new step does it. */
    assert(command(WORKFLOW_COMMAND_CONFIRM, false, false) == WORKFLOW_COMMAND_RESULT_OK);
    s_now_ms += 100U;
    assert(workflow_service_poll() == ESP_OK);
    assert(try_snapshot().step == WORKFLOW_STEP_SCAN_CHOICE);
    assert(workflow_test_scan_calls > reads_on_entry);
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

static void test_poll_shutdown_ack_precedes_resource_destroy(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_poll_barrier_lock) == 0);
#endif
    s_poll_barrier_enabled = true;
    s_poll_barrier_entered = false;
    s_poll_barrier_released = false;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_unlock(&s_poll_barrier_lock) == 0);
#endif

    const unsigned queue_deletes_before = workflow_host_rtos_queue_delete_count();
    const unsigned semaphore_deletes_before = s_host_semaphore_delete_calls;
    const unsigned external_deletes_before = workflow_host_rtos_external_delete_count();
    const unsigned ack_gives_before = s_poll_ack_give_calls;
    workflow_host_rtos_start_created_task();

#ifdef _WIN32
    AcquireSRWLockExclusive(&s_poll_barrier_lock);
    while (!s_poll_barrier_entered) {
        assert(SleepConditionVariableSRW(
            &s_poll_barrier_changed, &s_poll_barrier_lock, INFINITE, 0U));
    }
    ReleaseSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_poll_barrier_lock) == 0);
    while (!s_poll_barrier_entered) {
        assert(pthread_cond_wait(&s_poll_barrier_changed, &s_poll_barrier_lock) == 0);
    }
    assert(pthread_mutex_unlock(&s_poll_barrier_lock) == 0);
#endif

    assert(workflow_service_shutdown() == ESP_ERR_TIMEOUT);
    const unsigned queue_deletes_before_ack = workflow_host_rtos_queue_delete_count();
    const unsigned semaphore_deletes_before_ack = s_host_semaphore_delete_calls;
    const unsigned external_deletes_before_ack = workflow_host_rtos_external_delete_count();

#ifdef _WIN32
    AcquireSRWLockExclusive(&s_poll_barrier_lock);
    s_poll_barrier_released = true;
    WakeAllConditionVariable(&s_poll_barrier_changed);
    ReleaseSRWLockExclusive(&s_poll_barrier_lock);
#else
    assert(pthread_mutex_lock(&s_poll_barrier_lock) == 0);
    s_poll_barrier_released = true;
    assert(pthread_cond_broadcast(&s_poll_barrier_changed) == 0);
    assert(pthread_mutex_unlock(&s_poll_barrier_lock) == 0);
#endif
    workflow_host_rtos_join_created_task();

    assert(s_poll_ack_give_calls == ack_gives_before + 1U);
    assert(s_poll_ack_last_timeout == pdMS_TO_TICKS(APP_STOP_TIMEOUT_WORKER_MS));
    (void)workflow_service_shutdown();
    assert(queue_deletes_before_ack == queue_deletes_before);
    assert(semaphore_deletes_before_ack == semaphore_deletes_before);
    assert(external_deletes_before_ack == external_deletes_before);
    assert(workflow_host_rtos_queue_delete_count() == queue_deletes_before + 1U);
    assert(s_host_semaphore_delete_calls == semaphore_deletes_before + 3U);
    assert(workflow_host_rtos_external_delete_count() == external_deletes_before);
    assert(workflow_service_init() == ESP_OK);
    (void)workflow_service_shutdown();
    s_poll_barrier_enabled = false;
}

#if defined(MAX483B_CHECK_COUNTER)
/* fail-before(real): synthetic poll work overruns the configured period by 2.5x. */
static void test_workflow_poll_overrun_skips_expired_slots(void)
{
    reset_fakes(true, MEASUREMENT_SERVICE_LIVE, 4U);
    const TickType_t poll_period_ticks = pdMS_TO_TICKS(WORKFLOW_SERVICE_POLL_PERIOD_MS);
    s_next_poll_work_ticks = (poll_period_ticks * 5U) / 2U;
    workflow_host_rtos_delay_barrier_enable();

    const unsigned ack_gives_before = s_poll_ack_give_calls;
    workflow_host_rtos_start_created_task();
    workflow_host_rtos_delay_barrier_wait();

    assert(workflow_host_rtos_last_delay_period() == poll_period_ticks);
    assert(workflow_host_rtos_last_delay_wait() == poll_period_ticks / 2U);
    assert(workflow_service_test_poll_missed_deadline_count() == 2U);
    assert(workflow_service_shutdown() == ESP_ERR_TIMEOUT);

    workflow_host_rtos_delay_barrier_release();
    workflow_host_rtos_join_created_task();
    assert(s_poll_ack_give_calls == ack_gives_before + 1U);
    assert(workflow_service_shutdown() == ESP_OK);
    workflow_host_rtos_delay_barrier_disable();
}
#endif

int main(void)
{
#ifdef MAX449_CASE
#if MAX449_CASE == 4
    test_low_remote_calibration_timeout_and_pair_counts();
    test_one_and_two_connected_remotes_complete_initialization();
    test_rejected_imu_calibration_command_is_repeated_for_silent_nodes_only();
    test_low_remote_placement_and_ranging_finish();
    test_placement_becomes_stable_at_the_production_remote_refresh_cadence();
    test_placement_with_noisy_barometers_settles_and_reports_the_true_height();
    test_placement_reports_why_each_node_is_not_stable_and_logs_the_readings();
    test_placement_survives_a_missed_device_status_read_but_not_a_long_outage();
#else
#error "unknown MAX449_CASE for workflow_service_test"
#endif
#else
    test_sensor_check_requests_barometer_before_calibration();
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
    test_phone_gps_validates_and_persists_only_accepted_fixes();
    test_gps_failure_back_skip_and_late_callback_generation();
    test_gps_skip_keeps_existing_location_and_marks_run_skipped();
    test_scan_choice_requires_explicit_skip_before_recording();
    test_service_fsm_illegal_events_and_single_recording_lifecycle();
    test_new_workflow_steps_wait_for_explicit_commands();
    test_ranging_result_is_logged_with_every_pair_and_node();
    test_one_and_two_connected_remotes_complete_initialization();
    test_rejected_imu_calibration_command_is_repeated_for_silent_nodes_only();
    test_placement_becomes_stable_at_the_production_remote_refresh_cadence();
    test_placement_reports_why_each_node_is_not_stable_and_logs_the_readings();
    test_placement_survives_a_missed_device_status_read_but_not_a_long_outage();
    test_no_step_advances_without_a_command_fake_clock();
    test_staged_scan_remains_visible_across_session_creation();
    test_scan_presence_and_settings_refresh_are_cached_for_two_seconds();
    test_the_sd_card_is_left_alone_from_calibration_to_the_ranging_result();
    test_calc_settings_readiness_gates_calculate_and_save_invalidates_cache();
    test_calculation_success_progress_failure_classes_and_submit_errors();
    test_invalid_settings_at_submit_and_orphaned_job_completion();
    test_second_calculation_while_orphaned_is_busy_without_duplicate_submit();
    test_material_save_runs_in_poll_and_refreshes_settings_ready();
    test_material_save_reports_sd_error_and_missing_active_object();
    test_material_save_queue_full_is_rejected_without_sd_write();
    test_material_is_carried_into_run_provenance();
    test_poll_shutdown_ack_precedes_resource_destroy();
#if defined(MAX483B_CHECK_COUNTER)
    test_workflow_poll_overrun_skips_expired_slots();
#endif
    return 0;
#endif
}
