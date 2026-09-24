#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    DEVICE_POWER_SOURCE_UNKNOWN = 0,
    DEVICE_POWER_SOURCE_BATTERY,
    DEVICE_POWER_SOURCE_USB,
} device_power_source_t;

typedef enum {
    DEVICE_POWER_OVERCURRENT_NONE = 0,
    DEVICE_POWER_OVERCURRENT_WARNING,
    DEVICE_POWER_OVERCURRENT_CRITICAL,
} device_power_overcurrent_t;

esp_err_t device_status_set_power(float voltage_v, float current_a, bool valid);
esp_err_t device_status_set_power_channel_overcurrent(
    size_t channel_index,
    device_power_overcurrent_t overcurrent,
    bool latched
);
esp_err_t device_status_set_power_channel(
    size_t channel_index,
    float voltage_v,
    float current_a,
    bool valid
);
esp_err_t device_status_set_power_battery(
    float battery_voltage_v,
    float battery_current_a,
    float battery_percent_estimate,
    bool battery_present,
    bool battery_percent_valid,
    bool usb_present,
    bool charging,
    device_power_source_t source
);
