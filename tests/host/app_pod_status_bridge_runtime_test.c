#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_pod_status_bridge_internal.h"
#include "device_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nrf_topology_service.h"
#include "object_store.h"

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
static fake_position_t s_positions[DEVICE_STATUS_POD_COUNT];
static unsigned s_barometer_calls;
static unsigned s_position_calls;
static bool s_active_available;
static bool s_profile_available;
static object_store_summary_t s_active;
static object_store_profile_t s_profile;
static nrf_remote_sensor_cache_t s_sensor_cache[DEVICE_STATUS_POD_COUNT];
static esp_err_t s_sensor_errors[DEVICE_STATUS_POD_COUNT];

static void reset_fakes(void)
{
    memset(s_barometers, 0, sizeof(s_barometers));
    memset(s_positions, 0, sizeof(s_positions));
    memset(s_sensor_cache, 0, sizeof(s_sensor_cache));
    memset(s_sensor_errors, 0, sizeof(s_sensor_errors));
    s_barometer_calls = 0U;
    s_position_calls = 0U;
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
    s_sensor_cache[channel].topology_current = topology_current;
    s_sensor_cache[channel].sensor.valid_mask = environment_valid
        ? NRF_UPSTREAM_SENSOR_VALID_ENV
        : 0U;
    s_sensor_cache[channel].sensor.pressure_pa = pressure_pa;
    s_sensor_cache[channel].sensor.altitude_mm = altitude_mm;
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
    s_sensor_errors[3] = ESP_FAIL;

    app_pod_status_bridge_publish_cycle();

    assert(s_barometer_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_position_calls == DEVICE_STATUS_POD_COUNT);
    assert(s_barometers[0].valid);
    assert(s_barometers[0].pressure_pa == 100000.0f);
    assert(s_barometers[0].relative_altitude_cm == 125.0f);
    assert(!s_barometers[1].valid);
    assert(!s_barometers[2].valid);
    assert(!s_barometers[3].valid);
    assert(s_positions[0].valid);
    assert(s_positions[0].local_x_m == 1.25f);
    assert(s_positions[0].local_y_m == -0.5f);
    assert(!s_positions[1].valid);
    assert(s_positions[2].valid);
    assert(s_positions[2].local_x_m == -2.2f);
    assert(s_positions[2].local_y_m == 0.3f);
    assert(!s_positions[3].valid);
}

static void test_cycle_invalidates_local_position_when_active_profile_disappears(void)
{
    reset_fakes();
    s_profile.geometry.pods[0].computed = true;
    s_profile.geometry.pods[0].x_mm = 900;
    s_profile.geometry.pods[0].y_mm = 100;
    configure_sensor(0U, true, true, true, 99900U, 0);
    s_profile_available = false;

    app_pod_status_bridge_publish_cycle();

    assert(s_barometers[0].valid);
    for (size_t channel = 0U; channel < DEVICE_STATUS_POD_COUNT; ++channel) {
        assert(!s_positions[channel].valid);
        assert(s_positions[channel].local_x_m == 0.0f);
        assert(s_positions[channel].local_y_m == 0.0f);
    }
}

int main(void)
{
    test_cycle_publishes_fresh_sensor_and_frozen_geometry();
    test_cycle_invalidates_local_position_when_active_profile_disappears();
    return 0;
}
