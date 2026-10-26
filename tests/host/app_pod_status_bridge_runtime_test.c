#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_pod_status_bridge.h"
#include "app_pod_status_bridge_internal.h"
#include "device_status.h"
#include "esp_timer.h"
#include "hw_inventory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nrf_topology_service.h"
#include "object_store.h"

static object_store_summary_t s_active;

#if defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
#include "object_store_internal.h"
#include "storage_manager.h"

static char s_persisted_profile_json[2048];
static unsigned s_profile_storage_reads;
static unsigned s_profile_object_stats;
static unsigned s_profile_path_stats;
static unsigned s_settings_path_stats;

bool storage_manager_is_ready(void) { return true; }
void storage_manager_operation_end(void) {}
esp_err_t object_store_internal_begin_file_operation(void) { return ESP_OK; }
void object_store_internal_end_file_operation(void) {}
esp_err_t object_store_internal_begin_state_operation(void) { return ESP_OK; }
void object_store_internal_end_state_operation(void) {}
const char *object_store_internal_active_id(void) { return s_active.id; }

bool storage_manager_path_exists(const char *path)
{
    if (path != NULL && strstr(path, "/object.json") != NULL) {
        ++s_profile_path_stats;
        return true;
    }
    if (path != NULL && strstr(path, "/settings.json") != NULL) {
        ++s_settings_path_stats;
    }
    return false;
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL) return ESP_ERR_INVALID_ARG;
    if (strstr(path, "/object.json") == NULL) return ESP_ERR_NOT_FOUND;
    ++s_profile_storage_reads;
    const size_t length = strlen(s_persisted_profile_json) + 1U;
    if (length > buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, s_persisted_profile_json, length);
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    (void)path;
    (void)text;
    return ESP_OK;
}

bool object_store_fs_object_exists(const char *object_id)
{
    ++s_profile_object_stats;
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id)
{
    (void)objects;
    (void)max_objects;
    (void)active_object_id;
    *object_count = 0U;
    return ESP_OK;
}

#endif

int64_t esp_timer_get_time(void)
{
    static int64_t now_us;
    now_us += 1000;
    return now_us;
}

typedef struct {
    float pressure_pa;
    float relative_altitude_cm;
    float reference_pressure_pa;
    bool valid;
} fake_barometer_t;

typedef struct {
    float local_x_m;
    float local_y_m;
    bool valid;
} fake_position_t;

static fake_barometer_t s_barometers[DEVICE_STATUS_POD_COUNT];
static device_height_reference_status_t s_height_references[DEVICE_STATUS_POD_COUNT];
static fake_position_t s_positions[DEVICE_STATUS_POD_COUNT];
static pod_state_t s_pod_states[DEVICE_STATUS_POD_COUNT];
static unsigned s_barometer_calls;
typedef struct { float temperature_c; bool temperature_valid; float humidity_percent; bool humidity_valid; } fake_environment_t;
typedef struct { float rate_dps; bool valid; } fake_gyro_t;
static fake_environment_t s_environments[DEVICE_STATUS_POD_COUNT];
static fake_gyro_t s_gyros[DEVICE_STATUS_POD_COUNT];
static unsigned s_environment_calls;
static unsigned s_gyro_calls;
static unsigned s_height_reference_calls;
static unsigned s_position_calls;
static unsigned s_pod_state_calls;
static unsigned s_topology_snapshot_calls;
static bool s_active_available;
static bool s_profile_available;
static object_store_profile_t s_profile;
static nrf_topology_snapshot_t s_topology;
static esp_err_t s_topology_error;
static nrf_remote_sensor_cache_t s_sensor_cache[DEVICE_STATUS_POD_COUNT];
static esp_err_t s_sensor_errors[DEVICE_STATUS_POD_COUNT];
static unsigned s_warning_log_calls;

void app_pod_status_bridge_test_log(
    char level,
    const char *tag,
    const char *format,
    ...
)
{
    (void)tag;
    (void)format;
    if (level == 'W') {
        ++s_warning_log_calls;
    }
}

static void reset_fakes(void)
{
    hw_inventory_init();
    memset(s_barometers, 0, sizeof(s_barometers));
    memset(s_environments, 0, sizeof(s_environments));
    memset(s_gyros, 0, sizeof(s_gyros));
    s_environment_calls = 0U;
    s_gyro_calls = 0U;
    memset(s_height_references, 0, sizeof(s_height_references));
    memset(s_positions, 0, sizeof(s_positions));
    memset(s_pod_states, 0, sizeof(s_pod_states));
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    memset(&s_topology, 0, sizeof(s_topology));
    memset(s_sensor_errors, 0, sizeof(s_sensor_errors));
    s_barometer_calls = 0U;
    s_height_reference_calls = 0U;
    s_position_calls = 0U;
    s_pod_state_calls = 0U;
    s_topology_snapshot_calls = 0U;
    s_topology_error = ESP_OK;
    s_active_available = true;
    s_profile_available = true;
    memset(&s_active, 0, sizeof(s_active));
    s_active.active = true;
    (void)snprintf(s_active.id, sizeof(s_active.id), "OBJ_0001");
    memset(&s_profile, 0, sizeof(s_profile));
}

esp_err_t object_store_get_active(object_store_summary_t *object)
{
    if (!s_active_available || object == NULL) return ESP_ERR_NOT_FOUND;
    *object = s_active;
    return ESP_OK;
}

#if !defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
esp_err_t object_store_get_profile(
    const char *object_id,
    object_store_profile_t *profile
)
{
    if (!s_profile_available || object_id == NULL || profile == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *profile = s_profile;
    return ESP_OK;
}
#endif

esp_err_t nrf_topology_service_get_sensor_snapshot(
    uint8_t channel,
    nrf_remote_sensor_cache_t *snapshot
)
{
    if (snapshot == NULL || channel >= DEVICE_STATUS_POD_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sensor_errors[channel] != ESP_OK) return s_sensor_errors[channel];
    *snapshot = s_sensor_cache[channel];
    return ESP_OK;
}

esp_err_t object_store_get_profile_cached(
    const char *object_id,
    object_store_profile_t *profile
)
{
    return object_store_get_profile(object_id, profile);
}

esp_err_t nrf_topology_service_get_snapshot(nrf_topology_snapshot_t *snapshot)
{
    s_topology_snapshot_calls++;
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    if (s_topology_error != ESP_OK) return s_topology_error;
    *snapshot = s_topology;
    return ESP_OK;
}

esp_err_t device_status_set_pod_state(size_t pod_index, pod_state_t state)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_pod_state_calls++;
    s_pod_states[pod_index] = state;
    return ESP_OK;
}

static device_sensor_health_t s_sensor_health[DEVICE_STATUS_POD_COUNT];
esp_err_t device_status_set_pod_sensor_health(size_t pod_index, const device_sensor_health_t *health)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT && health != NULL);
    s_sensor_health[pod_index] = *health;
    return ESP_OK;
}

esp_err_t device_status_set_pod_barometer(
    size_t pod_index,
    float pressure_pa,
    float relative_altitude_cm,
    float reference_pressure_pa,
    bool valid
)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_barometer_calls++;
    s_barometers[pod_index] = (fake_barometer_t){
        pressure_pa,
        relative_altitude_cm,
        reference_pressure_pa,
        valid,
    };
    return ESP_OK;
}

esp_err_t device_status_set_pod_environment(
    size_t pod_index, float temperature_c, bool temperature_valid,
    float humidity_percent, bool humidity_valid)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_environment_calls++;
    s_environments[pod_index] = (fake_environment_t){
        temperature_c, temperature_valid, humidity_percent, humidity_valid};
    return ESP_OK;
}

esp_err_t device_status_set_pod_gyro_rate(size_t pod_index, float rate_dps, bool valid)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_gyro_calls++;
    s_gyros[pod_index] = (fake_gyro_t){rate_dps, valid};
    return ESP_OK;
}

esp_err_t device_status_set_pod_height_reference(
    size_t pod_index,
    device_height_reference_state_t state,
    uint16_t generation,
    int32_t relative_z_mm
)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_height_reference_calls++;
    s_height_references[pod_index] = (device_height_reference_status_t){
        state,
        generation,
        relative_z_mm,
    };
    return ESP_OK;
}

esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
)
{
    assert(pod_index < DEVICE_STATUS_POD_COUNT);
    s_position_calls++;
    s_positions[pod_index] = (fake_position_t){local_x_m, local_y_m, valid};
    return ESP_OK;
}

/* The lifecycle functions are linked because the production bridge contains
 * the task wrapper even though this test calls the real publication cycle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
UBaseType_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait)
{
    (void)clear_count_on_exit;
    (void)ticks_to_wait;
    return 0U;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task) { (void)task; return pdPASS; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t task,
    const char *name,
    unsigned stack_size,
    void *argument,
    unsigned priority,
    TaskHandle_t *task_handle,
    int core_id
)
{
    (void)task;
    (void)name;
    (void)stack_size;
    (void)argument;
    (void)priority;
    (void)task_handle;
    (void)core_id;
    return pdPASS;
}

static void configure_sensor(
    size_t channel,
    bool response_valid,
    bool topology_current,
    bool environment_valid,
    uint32_t pressure_pa,
    int32_t altitude_mm
)
{
    s_sensor_cache[channel].response_valid = response_valid;
    s_sensor_cache[channel].identity_valid = true;
    s_sensor_cache[channel].esp_receipt_uptime_ms = (uint64_t)esp_timer_get_time() / 1000U;
    s_sensor_cache[channel].topology_current = topology_current;
    s_sensor_cache[channel].sensor.valid_mask = environment_valid
        ? NRF_UPSTREAM_SENSOR_VALID_ENV
        : 0U;
    s_sensor_cache[channel].sensor.pressure_pa = pressure_pa;
    s_sensor_cache[channel].sensor.altitude_mm = altitude_mm;
}

static void assert_inventory_state(
    hw_module_id_t id,
    hw_module_state_t expected_state,
    const char *expected_reason)
{
    hw_module_info_t module;
    assert(hw_inventory_get(id, &module));
    assert(module.state == expected_state);
    assert(strcmp(module.reason, expected_reason) == 0);
}

static void test_scratch_failure_retries_and_successful_cycles_reuse_storage(void)
{
    reset_fakes();
    s_topology.generation = 1U;
    s_topology.tca_available = true;
    s_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    s_profile.geometry.pods[0].computed = true;
    s_profile.geometry.pods[0].x_mm = 1250;
    s_profile.geometry.pods[0].y_mm = -500;
    configure_sensor(0U, true, true, true, 100000U, 1250);
    s_sensor_cache[0].sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
    s_sensor_cache[0].sensor.reference_generation = 21U;
    s_sensor_cache[0].sensor.relative_z_mm = 750;

    app_pod_status_bridge_test_fail_next_scratch_allocations(3U);

    app_pod_status_bridge_publish_cycle();
    app_pod_status_bridge_publish_cycle();
    app_pod_status_bridge_publish_cycle();

    assert(app_pod_status_bridge_test_scratch_allocation_attempts() == 3U);
    assert(app_pod_status_bridge_test_scratch_allocation_count() == 0U);
    assert(s_warning_log_calls == 1U);
    assert(s_topology_snapshot_calls == 3U);
    assert(s_pod_state_calls == 3U * DEVICE_STATUS_POD_COUNT);
    assert(s_barometer_calls == 3U * DEVICE_STATUS_POD_COUNT);
    assert(s_height_reference_calls == 3U * DEVICE_STATUS_POD_COUNT);
    assert(s_position_calls == 3U * DEVICE_STATUS_POD_COUNT);
    assert(s_barometers[0].valid);
    assert(s_barometers[0].pressure_pa == 100000.0f);
    assert(s_height_references[0].state == DEVICE_HEIGHT_REFERENCE_VALID);
    assert(s_height_references[0].generation == 21U);
    assert(s_height_references[0].relative_z_mm == 750);
    assert(s_pod_states[0] == POD_STATE_READY);
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, "topology probe ok");
    assert(!s_positions[0].valid);
    assert(s_positions[0].local_x_m == 0.0f);
    assert(s_positions[0].local_y_m == 0.0f);

    app_pod_status_bridge_test_fail_next_scratch_allocations(0U);
    app_pod_status_bridge_publish_cycle();

    assert(app_pod_status_bridge_test_scratch_allocation_attempts() == 4U);
    assert(app_pod_status_bridge_test_scratch_allocation_count() == 1U);
    assert(s_positions[0].valid);
    assert(s_positions[0].local_x_m == 1.25f);
    assert(s_positions[0].local_y_m == -0.5f);

    const unsigned allocation_attempts =
        app_pod_status_bridge_test_scratch_allocation_attempts();
    const unsigned allocation_count =
        app_pod_status_bridge_test_scratch_allocation_count();
    for (unsigned cycle = 0U; cycle < 3U; ++cycle) {
        app_pod_status_bridge_publish_cycle();
        assert(s_positions[0].valid);
        assert(s_positions[0].local_x_m == 1.25f);
        assert(s_positions[0].local_y_m == -0.5f);
        assert(s_pod_states[0] == POD_STATE_READY);
    }
    assert(app_pod_status_bridge_test_scratch_allocation_attempts() == allocation_attempts);
    assert(app_pod_status_bridge_test_scratch_allocation_count() == allocation_count);

    s_active_available = false;
    app_pod_status_bridge_publish_cycle();
    assert(!s_positions[0].valid);
    assert(s_positions[0].local_x_m == 0.0f);
    assert(s_positions[0].local_y_m == 0.0f);
    assert(s_barometers[0].valid);

    s_active_available = true;
    s_profile_available = false;
    app_pod_status_bridge_publish_cycle();
    assert(!s_positions[0].valid);
    assert(s_positions[0].local_x_m == 0.0f);
    assert(s_positions[0].local_y_m == 0.0f);
    assert(app_pod_status_bridge_test_scratch_allocation_attempts() == allocation_attempts);
    assert(app_pod_status_bridge_test_scratch_allocation_count() == allocation_count);
    assert(s_warning_log_calls == 1U);
}


static void test_stop_releases_scratch_for_reallocation(void)
{
    reset_fakes();
    const unsigned allocations_before_stop =
        app_pod_status_bridge_test_scratch_allocation_count();
    const unsigned attempts_before_stop =
        app_pod_status_bridge_test_scratch_allocation_attempts();

    app_pod_status_bridge_stop();
    app_pod_status_bridge_publish_cycle();

    assert(app_pod_status_bridge_test_scratch_allocation_count() == allocations_before_stop + 1U);
    assert(app_pod_status_bridge_test_scratch_allocation_attempts() == attempts_before_stop + 1U);

    app_pod_status_bridge_stop();
}

static void test_cycle_publishes_fresh_sensor_and_frozen_geometry(void)
{
    reset_fakes();
    s_profile.geometry.pods[0].computed = true;
    s_profile.geometry.pods[0].x_mm = 1250;
    s_profile.geometry.pods[0].y_mm = -500;
    s_profile.geometry.pods[2].computed = true;
    s_profile.geometry.pods[2].x_mm = -2200;
    s_profile.geometry.pods[2].y_mm = 300;

    configure_sensor(0U, true, true, true, 100000U, 1250);
    configure_sensor(1U, true, false, true, 100100U, 900);
    configure_sensor(2U, true, true, false, 100200U, -350);
    configure_sensor(3U, true, false, true, 100300U, 0);
    s_sensor_cache[0].sensor.reference_state = NRF_UPSTREAM_REFERENCE_VALID;
    s_sensor_cache[0].sensor.reference_generation = 11U;
    s_sensor_cache[0].sensor.relative_z_mm = 2600;
    s_sensor_cache[1].sensor.reference_state = NRF_UPSTREAM_REFERENCE_REJECTED_MOVING;
    s_sensor_cache[1].sensor.reference_generation = 12U;
    s_sensor_cache[2].sensor.reference_state = NRF_UPSTREAM_REFERENCE_SENSOR_INVALID;
    s_sensor_cache[2].sensor.reference_generation = 13U;
    s_sensor_cache[3].sensor.reference_state = NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED;
    s_sensor_cache[3].sensor.reference_generation = 14U;

    app_pod_status_bridge_publish_cycle();

    assert(s_barometer_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_height_reference_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_position_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_topology_snapshot_calls == 1U);
    assert(s_pod_state_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_barometers[0].valid);
    assert(s_barometers[0].pressure_pa == 100000.0f);
    assert(s_barometers[0].relative_altitude_cm == 125.0f);
    assert(!s_barometers[1].valid);
    assert(!s_barometers[2].valid);
    assert(!s_barometers[3].valid);
    assert(s_height_references[0].state == DEVICE_HEIGHT_REFERENCE_VALID);
    assert(s_height_references[0].generation == 11U);
    assert(s_height_references[0].relative_z_mm == 2600);
    assert(s_height_references[1].state == DEVICE_HEIGHT_REFERENCE_REJECTED_MOVING);
    assert(s_height_references[1].generation == 12U);
    assert(s_height_references[1].relative_z_mm == 0);
    assert(s_height_references[2].state == DEVICE_HEIGHT_REFERENCE_SENSOR_INVALID);
    assert(s_height_references[2].generation == 13U);
    assert(s_height_references[2].relative_z_mm == 0);
    assert(s_height_references[3].state == DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED);
    assert(s_height_references[3].generation == 14U);
    assert(s_height_references[3].relative_z_mm == 0);
    assert(s_positions[0].valid);
    assert(s_positions[0].local_x_m == 1.25f);
    assert(s_positions[0].local_y_m == -0.5f);
    assert(!s_positions[1].valid);
    assert(s_positions[2].valid);
    assert(s_positions[2].local_x_m == -2.2f);
    assert(s_positions[2].local_y_m == 0.3f);
    assert(!s_positions[3].valid);
}

static void test_cycle_publishes_measured_temperature_humidity_and_gyro(void)
{
    reset_fakes();
    configure_sensor(0U, true, true, true, 100000U, 1250);
    s_sensor_cache[0].sensor.valid_mask |= NRF_UPSTREAM_SENSOR_VALID_IMU;
    s_sensor_cache[0].sensor.env_temperature_milli_c = 24250;
    s_sensor_cache[0].sensor.humidity_milli_percent = 48125U;
    s_sensor_cache[0].sensor.gyro_mdps[0] = 3000;
    s_sensor_cache[0].sensor.gyro_mdps[1] = 4000;
    /* Pod 1: no valid domains. Pod 2: environment only. */
    configure_sensor(1U, true, true, false, 0U, 0);
    configure_sensor(2U, true, true, true, 100200U, 0);
    s_sensor_cache[2].sensor.env_temperature_milli_c = -5500;
    s_sensor_cache[3].topology_current = false;

    app_pod_status_bridge_publish_cycle();

    assert(s_environment_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_gyro_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_environments[0].temperature_valid && s_environments[0].temperature_c == 24.25f);
    assert(s_environments[0].humidity_valid && s_environments[0].humidity_percent == 48.125f);
    assert(s_gyros[0].valid && s_gyros[0].rate_dps > 4.999f && s_gyros[0].rate_dps < 5.001f);
    assert(!s_environments[1].temperature_valid && !s_gyros[1].valid);
    assert(s_environments[2].temperature_valid && s_environments[2].temperature_c == -5.5f);
    assert(!s_gyros[2].valid);
    assert(!s_environments[3].temperature_valid && !s_gyros[3].valid);
}

static void test_stale_snapshot_does_not_keep_showing_measured_values(void)
{
    reset_fakes();
    configure_sensor(0U, true, true, true, 100000U, 0);
    s_sensor_cache[0].sensor.valid_mask |= NRF_UPSTREAM_SENSOR_VALID_IMU;
    s_sensor_cache[0].sensor.env_temperature_milli_c = 24250;
    s_sensor_cache[0].sensor.gyro_mdps[0] = 1000;
    /* The cache says valid, but the ESP last heard from this pod long ago. */
    s_sensor_cache[0].esp_receipt_uptime_ms = 0U;
    /* The fake clock advances 1 ms per call: age the receipt past the stale limit. */
    for (unsigned i = 0U; i < 7000U; ++i) (void)esp_timer_get_time();

    app_pod_status_bridge_publish_cycle();

    assert(!s_environments[0].temperature_valid);
    assert(!s_gyros[0].valid);
}

static void test_cycle_invalidates_local_position_when_active_profile_disappears(void)
{
    reset_fakes();
    s_profile.geometry.pods[0].computed = true;
    s_profile.geometry.pods[0].x_mm = 900;
    s_profile.geometry.pods[0].y_mm = 100;
    configure_sensor(0U, true, true, true, 99900U, 0);
    s_sensor_errors[3] = ESP_FAIL;
    s_profile_available = false;

    app_pod_status_bridge_publish_cycle();

    assert(s_barometers[0].valid);
    assert(s_height_references[3].state == DEVICE_HEIGHT_REFERENCE_NOT_ESTABLISHED);
    assert(s_height_references[3].generation == 0U);
    assert(s_height_references[3].relative_z_mm == 0);
    for (size_t channel = 0U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        assert(!s_positions[channel].valid);
        assert(s_positions[channel].local_x_m == 0.0f);
        assert(s_positions[channel].local_y_m == 0.0f);
    }
}

static void test_cycle_publishes_topology_pod_connection_states(void)
{
    reset_fakes();
    s_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    s_topology.remotes[1].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
    s_topology.remotes[2].state = NRF_TOPOLOGY_NODE_CONFLICT;
    s_topology.remotes[3].state = NRF_TOPOLOGY_NODE_ABSENT;

    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 1U);
    assert(s_pod_state_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_pod_states[0] == POD_STATE_READY);
    assert(s_pod_states[1] == POD_STATE_DISCONNECTED);
    assert(s_pod_states[2] == POD_STATE_ERROR);
    assert(s_pod_states[3] == POD_STATE_DISCONNECTED);

    s_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ABSENT;
    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 2U);
    assert(s_pod_state_calls == 2U * DEVICE_STATUS_POD_COUNT);
    assert(s_pod_states[0] == POD_STATE_DISCONNECTED);

    const pod_state_t previously_published[DEVICE_STATUS_POD_COUNT] = {
        s_pod_states[0],
        s_pod_states[1],
        s_pod_states[2],
        s_pod_states[3],
    };
    const unsigned previous_pod_state_calls = s_pod_state_calls;
    const unsigned previous_barometer_calls = s_barometer_calls;
    const unsigned previous_height_reference_calls = s_height_reference_calls;
    const unsigned previous_position_calls = s_position_calls;

    s_topology_error = ESP_FAIL;
    s_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 3U);
    assert(s_pod_state_calls == previous_pod_state_calls);
    for (size_t channel = 0U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        assert(s_pod_states[channel] == previously_published[channel]);
    }
    assert(s_barometer_calls == previous_barometer_calls + DEVICE_STATUS_POD_COUNT);
    assert(s_height_reference_calls == previous_height_reference_calls + DEVICE_STATUS_POD_COUNT);
    assert(s_position_calls == previous_position_calls + DEVICE_STATUS_POD_COUNT);
}

static void test_bus_fault_does_not_publish_four_connected_pods(void)
{
    reset_fakes();
    s_topology.generation = 1U;
    s_topology.tca_fault = true;
    for (size_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        s_topology.remotes[channel].state = NRF_TOPOLOGY_NODE_TRANSPORT_ERROR;
    }
    app_pod_status_bridge_publish_cycle();
    for (size_t channel = 0U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        assert(s_pod_states[channel] == POD_STATE_DISCONNECTED);
    }
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_FAULT, "topology probe fault");

    s_topology.tca_fault = false;
    s_topology.tca_available = true;
    s_topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    app_pod_status_bridge_publish_cycle();
    assert(s_pod_states[0] == POD_STATE_READY);
    for (size_t channel = 1U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        assert(s_pod_states[channel] == POD_STATE_DISCONNECTED);
    }
}

static void test_cycle_updates_topology_inventory_from_existing_snapshot(void)
{
    reset_fakes();
    s_topology.generation = 1U;
    s_topology.tca_available = true;
    s_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;

    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 1U);
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, "topology probe ok");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_PRESENT, "upstream target assigned");

    s_topology.generation = 2U;
    s_topology.tca_available = false;
    s_topology.main.state = NRF_TOPOLOGY_NODE_ABSENT;
    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 2U);
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_ABSENT, "no response");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_ABSENT, "no upstream response");

    s_topology.generation = 3U;
    s_topology.tca_available = true;
    s_topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    app_pod_status_bridge_publish_cycle();

    assert(s_topology_snapshot_calls == 3U);
    assert_inventory_state(HW_MODULE_SENSOR_MUX, HW_STATE_PRESENT, "topology probe ok");
    assert_inventory_state(HW_MODULE_MAIN_XIAO, HW_STATE_PRESENT, "upstream target assigned");
}

#if defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
static void test_pod_bridge_profile_cache_stops_storage_work_after_warm_cycle(void)
{
    reset_fakes();
    object_store_profile_cache_reset();
    (void)snprintf(
        s_persisted_profile_json,
        sizeof(s_persisted_profile_json),
        "{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\","
        "\"geometry\":{\"pods\":[{\"computed\":true,\"x_mm\":1000,"
        "\"y_mm\":-500,\"z_mm\":0}]}}"
    );
    s_profile_storage_reads = 0U;
    s_profile_object_stats = 0U;
    s_profile_path_stats = 0U;
    s_settings_path_stats = 0U;

    app_pod_status_bridge_publish_cycle();
    assert(s_profile_storage_reads == 1U);
    assert(s_profile_object_stats == 1U);
    assert(s_profile_path_stats == 1U);
    const unsigned reads_after_warm = s_profile_storage_reads;
    const unsigned stats_after_warm = s_profile_object_stats;
    const unsigned path_stats_after_warm = s_profile_path_stats;
    const unsigned settings_stats_after_warm = s_settings_path_stats;

    for (unsigned cycle = 0U; cycle < 6U; ++cycle) {
        app_pod_status_bridge_publish_cycle();
        assert(s_positions[0].valid);
        assert(s_positions[0].local_x_m == 1.0f);
        assert(s_positions[0].local_y_m == -0.5f);
    }
    assert(s_profile_storage_reads == reads_after_warm);
    assert(s_profile_object_stats == stats_after_warm);
    assert(s_profile_path_stats == path_stats_after_warm);
    assert(s_settings_path_stats == settings_stats_after_warm);
}
#endif

#if defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
static void (*const s_bridge_fallback_tests[])(void) = {
    test_scratch_failure_retries_and_successful_cycles_reuse_storage,
    test_cycle_publishes_fresh_sensor_and_frozen_geometry,
    test_cycle_invalidates_local_position_when_active_profile_disappears,
    test_cycle_publishes_topology_pod_connection_states,
    test_bus_fault_does_not_publish_four_connected_pods,
    test_cycle_updates_topology_inventory_from_existing_snapshot,
    test_stop_releases_scratch_for_reallocation,
};
#endif

int main(void)
{
#if defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
    assert(s_bridge_fallback_tests[0] != NULL);
    test_pod_bridge_profile_cache_stops_storage_work_after_warm_cycle();
#else
    test_scratch_failure_retries_and_successful_cycles_reuse_storage();
    test_cycle_publishes_fresh_sensor_and_frozen_geometry();
    test_cycle_publishes_measured_temperature_humidity_and_gyro();
    test_stale_snapshot_does_not_keep_showing_measured_values();
    test_cycle_invalidates_local_position_when_active_profile_disappears();
    test_cycle_publishes_topology_pod_connection_states();
    test_bus_fault_does_not_publish_four_connected_pods();
    test_cycle_updates_topology_inventory_from_existing_snapshot();
    test_stop_releases_scratch_for_reallocation();
#endif
    return 0;
}

#if defined(APP_POD_BRIDGE_REAL_PROFILE_CACHE_TEST)
#include "../../components/object_store/object_store_profile.c"
#endif
