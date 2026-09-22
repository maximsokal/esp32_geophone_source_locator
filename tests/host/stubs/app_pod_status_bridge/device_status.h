#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define DEVICE_STATUS_POD_COUNT 4

esp_err_t device_status_set_pod_barometer(
    size_t pod_index,
    float pressure_pa,
    float relative_altitude_cm,
    float reference_pressure_pa,
    bool valid
);

esp_err_t device_status_set_pod_local_position(
    size_t pod_index,
    float local_x_m,
    float local_y_m,
    bool valid
);
