#include <assert.h>
#include <math.h>
#include <string.h>

#include "device_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static int s_mutex_token;
static bool s_fail_mutex;
static unsigned s_take_calls;
static unsigned s_give_calls;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return s_fail_mutex ? NULL : &s_mutex_token;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    assert(semaphore == &s_mutex_token);
    assert(ticks == portMAX_DELAY);
    ++s_take_calls;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore == &s_mutex_token);
    ++s_give_calls;
    return pdTRUE;
}

int main(void)
{
    device_status_snapshot_t snapshot;
    memset(&snapshot, 0xA5, sizeof(snapshot));

    assert(device_status_get_snapshot(&snapshot) == ESP_ERR_INVALID_STATE);
    s_fail_mutex = true;
    assert(device_status_init() == ESP_ERR_NO_MEM);
    s_fail_mutex = false;
    assert(device_status_init() == ESP_OK);
    assert(device_status_init() == ESP_OK);
    assert(device_status_get_snapshot(NULL) == ESP_ERR_INVALID_ARG);
    assert(device_status_get_snapshot(&snapshot) == ESP_OK);

    assert(snapshot.mode == DEVICE_MODE_IDLE);
    assert(snapshot.power.source == DEVICE_POWER_SOURCE_UNKNOWN);
    assert(!snapshot.mux.valid && snapshot.mux.active_channel == -1 && snapshot.mux.active_pod == -1);
    for (size_t i = 0; i < DEVICE_STATUS_POD_COUNT; ++i) {
        assert(snapshot.pods[i].state == POD_STATE_DISCONNECTED);
    }

    assert(device_status_set_power(5.0f, 2.0f, true) == ESP_OK);
    assert(device_status_set_power_channel(1, 4.9f, 0.5f, true) == ESP_OK);
    assert(device_status_set_power_channel(DEVICE_STATUS_POWER_CHANNEL_COUNT, 1.0f, 1.0f, true) == ESP_ERR_INVALID_ARG);
    /* MAX-245 over-current state per channel. */
    assert(device_status_set_power_channel_overcurrent(2, DEVICE_POWER_OVERCURRENT_CRITICAL, true) == ESP_OK);
    assert(device_status_set_power_channel_overcurrent(DEVICE_STATUS_POWER_CHANNEL_COUNT, DEVICE_POWER_OVERCURRENT_NONE, false) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_power_channel_overcurrent(0, (device_power_overcurrent_t)7, false) == ESP_ERR_INVALID_ARG);
    {
        device_status_snapshot_t oc_snapshot;
        assert(device_status_get_snapshot(&oc_snapshot) == ESP_OK);
        assert(oc_snapshot.power.channels[2].overcurrent == DEVICE_POWER_OVERCURRENT_CRITICAL);
        assert(oc_snapshot.power.channels[2].overcurrent_latched);
        assert(oc_snapshot.power.channels[0].overcurrent == DEVICE_POWER_OVERCURRENT_NONE);
        assert(strcmp(device_status_power_overcurrent_name(DEVICE_POWER_OVERCURRENT_WARNING), "warning") == 0);
    }
    assert(device_status_set_power_battery(4.1f, 0.2f, 80.0f, true, true, false, false, DEVICE_POWER_SOURCE_BATTERY) == ESP_OK);
    assert(device_status_set_power_battery(0, 0, 0, false, false, false, false, (device_power_source_t)99) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_environment(22.5f, 48.0f, 100500.0f, true) == ESP_OK);
    assert(device_status_set_mux(7, 3, true) == ESP_OK);
    assert(device_status_set_mux(8, 0, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_mux(0, 4, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_mode(DEVICE_MODE_MEASUREMENT) == ESP_OK);
    assert(device_status_set_acquisition(true, 500, 1234, 2500) == ESP_OK);

    assert(device_status_set_pod_state(0, POD_STATE_READY) == ESP_OK);
    assert(device_status_set_pod_state(DEVICE_STATUS_POD_COUNT, POD_STATE_READY) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_barometer(0, 100000.0f, 12.5f, 101325.0f, true) == ESP_OK);
    assert(device_status_set_pod_imu(0, 1.0f, 2.0f, 3.0f, true) == ESP_OK);
    /* Gyro rate is its own measurement: it does not touch roll/pitch/yaw or capabilities. */
    assert(device_status_set_pod_gyro_rate(1, 0.25f, true) == ESP_OK);
    assert(device_status_set_pod_gyro_rate(1, -0.1f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_gyro_rate(1, NAN, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_gyro_rate(DEVICE_STATUS_POD_COUNT, 0.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_gyro_rate(2, 7.0f, true) == ESP_OK);
    assert(device_status_set_pod_gyro_rate(2, 7.0f, false) == ESP_OK);
    const device_sensor_health_t health = {
        .current = true, .imu_valid = true, .barometer_fault = true,
    };
    assert(device_status_set_pod_sensor_health(0, &health) == ESP_OK);
    assert(device_status_set_pod_sensor_health(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_sensor_health(DEVICE_STATUS_POD_COUNT, &health) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_environment(0, 21.0f, true, 55.0f, true) == ESP_OK);
    assert(device_status_set_pod_environment(0, 21.0f, true, 101.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_environment(0, NAN, true, 55.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_local_position(0, 1.25f, -0.5f, true) == ESP_OK);
    assert(device_status_set_pod_local_position(0, NAN, 0.0f, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_global_position(0, 48.0, 23.0, 220.0, true, true) == ESP_OK);
    assert(device_status_set_pod_global_position(0, 91.0, 23.0, 0.0, false, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_global_position(0, 48.0, 181.0, 0.0, false, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_global_position(0, 48.0, 23.0, NAN, true, true) == ESP_ERR_INVALID_ARG);
    assert(device_status_set_pod_geophone(0, 10.0f, 4.0f, 12.0f, 1.0f, 20.0f, true) == ESP_OK);

    assert(device_status_get_snapshot(&snapshot) == ESP_OK);
    assert(snapshot.power.valid && fabsf(snapshot.power.power_w - 10.0f) < 0.001f);
    assert(snapshot.pods[0].sensor_health.current && snapshot.pods[0].sensor_health.imu_valid);
    assert(snapshot.pods[0].sensor_health.barometer_fault && !snapshot.pods[0].sensor_health.imu_calibrated);
    assert(snapshot.power.channels[1].valid && fabsf(snapshot.power.channels[1].power_w - 2.45f) < 0.001f);
    assert(snapshot.power.source == DEVICE_POWER_SOURCE_BATTERY && snapshot.power.battery_percent_valid);
    assert(snapshot.environment.valid && snapshot.environment.temperature_c == 22.5f);
    assert(snapshot.mux.valid && snapshot.mux.active_channel == 7 && snapshot.mux.active_pod == 3);
    assert(snapshot.mode == DEVICE_MODE_MEASUREMENT);
    assert(snapshot.acquisition.running && snapshot.acquisition.sample_rate_hz == 500 && snapshot.acquisition.samples_recorded == 1234);
    assert(snapshot.pods[0].state == POD_STATE_READY);
    assert(snapshot.pods[0].capabilities.barometer && snapshot.pods[0].barometer.valid);
    assert(snapshot.pods[0].capabilities.imu && snapshot.pods[0].imu.valid);
    assert(!snapshot.pods[0].imu.gyro_valid && snapshot.pods[0].imu.gyro_rate_dps == 0.0f);
    assert(snapshot.pods[1].imu.gyro_valid && snapshot.pods[1].imu.gyro_rate_dps == 0.25f);
    assert(!snapshot.pods[1].capabilities.imu && !snapshot.pods[1].imu.valid);
    assert(!snapshot.pods[2].imu.gyro_valid && snapshot.pods[2].imu.gyro_rate_dps == 0.0f);
    assert(snapshot.pods[0].capabilities.environment && snapshot.pods[0].environment.valid);
    assert(snapshot.pods[0].capabilities.local_position && snapshot.pods[0].position.local_valid);
    assert(snapshot.pods[0].capabilities.global_position && snapshot.pods[0].position.global_valid && snapshot.pods[0].position.altitude_valid);
    assert(snapshot.pods[0].capabilities.geophone && snapshot.pods[0].geophone.valid);
    assert(strcmp(device_status_mode_name(DEVICE_MODE_WIFI), "WIFI") == 0);
    assert(strcmp(device_status_power_source_name((device_power_source_t)99), "UNKNOWN") == 0);
    assert(strcmp(device_status_pod_name((pod_state_t)99), "DISCONNECTED") == 0);
    assert(strcmp(device_status_pod_short_name(POD_STATE_WARNING), "!!") == 0);
    assert(s_take_calls == s_give_calls);

    return 0;
}
