#include "device_status.h"

#include <string.h>

#include "device_status_internal.h"

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


    s_mutex =
        xSemaphoreCreateMutex();


    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }


    memset(
        &s_status,
        0,
        sizeof(s_status)
    );


    s_status.mode =
        DEVICE_MODE_IDLE;

    s_status.power.source =
        DEVICE_POWER_SOURCE_UNKNOWN;


    s_status.mux.valid =
        false;

    s_status.mux.active_channel =
        -1;

    s_status.mux.active_pod =
        -1;


    for (
        size_t i = 0;
        i < DEVICE_STATUS_POD_COUNT;
        ++i
    ) {
        s_status.pods[i].state =
            POD_STATE_DISCONNECTED;
    }


    s_initialized =
        true;


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


    *snapshot =
        s_status;


    xSemaphoreGive(
        s_mutex
    );


    return ESP_OK;
}


esp_err_t device_status_internal_lock(
    device_status_snapshot_t **status
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    xSemaphoreTake(
        s_mutex,
        portMAX_DELAY
    );


    *status =
        &s_status;


    return ESP_OK;
}


void device_status_internal_unlock(void)
{
    if (
        !s_initialized ||
        s_mutex == NULL
    ) {
        return;
    }


    xSemaphoreGive(
        s_mutex
    );
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


const char *device_status_power_source_name(
    device_power_source_t source
)
{
    switch (source) {

        case DEVICE_POWER_SOURCE_BATTERY:
            return "BATTERY";

        case DEVICE_POWER_SOURCE_USB:
            return "USB";

        case DEVICE_POWER_SOURCE_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}


const char *device_status_pod_name(
    pod_state_t state
)
{
    switch (state) {

        case POD_STATE_READY:
            return "READY";

        case POD_STATE_WARNING:
            return "WARNING";

        case POD_STATE_ERROR:
            return "ERROR";

        case POD_STATE_DISCONNECTED:
        default:
            return "DISCONNECTED";
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
