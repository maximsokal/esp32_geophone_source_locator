#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


#define DEVICE_STATUS_POD_COUNT 4


typedef enum
{
    DEVICE_MODE_IDLE = 0,
    DEVICE_MODE_MEASUREMENT,
    DEVICE_MODE_WIFI
} device_mode_t;


typedef enum
{
    POD_STATE_DISCONNECTED = 0,
    POD_STATE_READY,
    POD_STATE_WARNING,
    POD_STATE_ERROR
} pod_state_t;


typedef struct
{
    bool power_valid;

    float battery_voltage_v;
    float battery_current_a;

    device_mode_t mode;

    pod_state_t pods[DEVICE_STATUS_POD_COUNT];

} device_status_snapshot_t;


esp_err_t device_status_init(void);


esp_err_t device_status_get_snapshot(
    device_status_snapshot_t *snapshot
);


esp_err_t device_status_set_power(
    float voltage_v,
    float current_a,
    bool valid
);


esp_err_t device_status_set_mode(
    device_mode_t mode
);


esp_err_t device_status_set_pod_state(
    size_t pod_index,
    pod_state_t state
);


const char *device_status_mode_name(
    device_mode_t mode
);


const char *device_status_pod_short_name(
    pod_state_t state
);