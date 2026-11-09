#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DEVICE_MAIN_HEIGHT_REFERENCE_NOT_ESTABLISHED = 0,
    DEVICE_MAIN_HEIGHT_REFERENCE_VALID,
    DEVICE_MAIN_HEIGHT_REFERENCE_REJECTED_UNSTABLE,
    DEVICE_MAIN_HEIGHT_REFERENCE_SENSOR_INVALID,
} device_main_height_reference_state_t;

typedef struct {
    device_main_height_reference_state_t state;
    uint16_t generation;
    int32_t relative_z_mm;
} device_main_height_reference_status_t;

typedef struct {
    bool bmp280_valid;
    uint32_t pressure_pa;
    int32_t temperature_mc;
    uint32_t barometer_sample_ms;
    int32_t barometer_filtered_altitude_mm;
    device_main_height_reference_status_t height_reference;
} device_main_status_t;

typedef struct {
    device_main_status_t main;
} device_status_snapshot_t;

esp_err_t device_status_try_get_snapshot(device_status_snapshot_t *snapshot);
