#include "device_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


static SemaphoreHandle_t s_mutex = NULL;

static device_status_snapshot_t s_status;

static bool s_initialized = false;


esp_err_t device_status_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();

    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(
        &s_status,
        0,
        sizeof(s_status)
    );

    s_status.power_valid = false;
    s_status.mode = DEVICE_MODE_IDLE;

    for (size_t i = 0; i < DEVICE_STATUS_POD_COUNT; i++) {
        s_status.pods[i] = POD_STATE_DISCONNECTED;
    }

    s_initialized = true;

    return ESP_OK;
}


esp_err_t device_status_get_snapshot(
    device_status_snapshot_t *snapshot
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    *snapshot = s_status;

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}


esp_err_t device_status_set_power(
    float voltage_v,
    float current_a,
    bool valid
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    s_status.battery_voltage_v = voltage_v;
    s_status.battery_current_a = current_a;
    s_status.power_valid = valid;

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}


esp_err_t device_status_set_mode(
    device_mode_t mode
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    s_status.mode = mode;

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}


esp_err_t device_status_set_pod_state(
    size_t pod_index,
    pod_state_t state
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pod_index >= DEVICE_STATUS_POD_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );

    s_status.pods[pod_index] = state;

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}


const char *device_status_mode_name(
    device_mode_t mode
)
{
    switch (mode) {

        case DEVICE_MODE_MEASUREMENT:
            return "MEASURE";

        case DEVICE_MODE_WIFI:
            return "WIFI";

        case DEVICE_MODE_IDLE:
        default:
            return "IDLE";
    }
}


const char *device_status_pod_short_name(
    pod_state_t state
)
{
    switch (state) {

        case POD_STATE_READY:
            return "OK";

        case POD_STATE_WARNING:
            return "!!";

        case POD_STATE_ERROR:
            return "ER";

        case POD_STATE_DISCONNECTED:
        default:
            return "--";
    }
}