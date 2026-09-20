#include "app_pod_status_bridge.h"

#include <stdbool.h>

#include "app_pod_status_barometer_policy.h"
#include "app_task_config.h"
#include "device_status.h"
#include "nrf_topology_service.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#define APP_POD_STATUS_BRIDGE_PERIOD_MS 2000U
#define APP_POD_STATUS_BRIDGE_REFERENCE_PRESSURE_PA 101325.0f


static const char *TAG =
    "POD_STATUS_BRIDGE";

static TaskHandle_t s_task =
    NULL;

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_starting =
    false;

static bool s_stop_requested =
    false;


static bool bridge_stop_requested(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}


static bool bridge_register_current_task(void)
{
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    bool stop_before_start;
    portENTER_CRITICAL(&s_state_lock);
    stop_before_start = s_stop_requested;
    s_task = task;
    s_starting = false;
    portEXIT_CRITICAL(&s_state_lock);
    return stop_before_start;
}


static void bridge_clear_current_task(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_task = NULL;
    portEXIT_CRITICAL(&s_state_lock);
}


static void publish_channel_barometer(uint8_t channel)
{
    nrf_remote_sensor_cache_t cache;
    const esp_err_t err =
        nrf_topology_service_get_sensor_snapshot(channel, &cache);

    const app_pod_status_barometer_reading_t reading =
        app_pod_status_bridge_compute_barometer_reading(
            err == ESP_OK ? &cache : NULL);

    (void)device_status_set_pod_barometer(
        channel,
        reading.pressure_pa,
        reading.relative_altitude_cm,
        APP_POD_STATUS_BRIDGE_REFERENCE_PRESSURE_PA,
        reading.valid);
}


static void bridge_task(void *argument)
{
    (void)argument;

    const bool stop_before_start =
        bridge_register_current_task();

    while (!stop_before_start && !bridge_stop_requested()) {
        for (
            uint8_t channel = 0U;
            channel < DEVICE_STATUS_POD_COUNT;
            ++channel
        ) {
            publish_channel_barometer(channel);
        }

        const uint32_t notifications =
            ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(APP_POD_STATUS_BRIDGE_PERIOD_MS));

        if (notifications > 0U && bridge_stop_requested()) {
            break;
        }
    }

    bridge_clear_current_task();
    vTaskDelete(NULL);
}


esp_err_t app_pod_status_bridge_start(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_stop_requested) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL || s_starting) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }
    s_starting = true;
    portEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t task = NULL;
    const BaseType_t result =
        xTaskCreatePinnedToCore(
            bridge_task,
            "pod_status_bridge",
            APP_TASK_STACK_POD_STATUS_BRIDGE,
            NULL,
            APP_TASK_PRIORITY_POD_STATUS_BRIDGE,
            &task,
            APP_TASK_CORE_SERVICES);

    if (result != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_starting = false;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Pod barometer status bridge started on core=%d",
        APP_TASK_CORE_SERVICES);
    return ESP_OK;
}


void app_pod_status_bridge_stop(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = true;
    const TaskHandle_t task = s_task;
    const bool starting = s_starting;
    portEXIT_CRITICAL(&s_state_lock);

    if (task == NULL && !starting) {
        portENTER_CRITICAL(&s_state_lock);
        s_stop_requested = false;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    if (task != NULL) {
        (void)xTaskNotifyGive(task);
    }

    while (true) {
        portENTER_CRITICAL(&s_state_lock);
        const bool stopped = s_task == NULL && !s_starting;
        portEXIT_CRITICAL(&s_state_lock);
        if (stopped) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    portENTER_CRITICAL(&s_state_lock);
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
}
