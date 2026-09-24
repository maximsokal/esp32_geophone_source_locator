#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

typedef enum {
    DEVICE_POWER_STATE_UNKNOWN = 0,
    DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY,
    DEVICE_POWER_STATE_USB_CHARGING,
    DEVICE_POWER_STATE_USB_BATTERY_READY,
    DEVICE_POWER_STATE_BATTERY,
    DEVICE_POWER_STATE_BATTERY_LOW,
    DEVICE_POWER_STATE_BATTERY_CRITICAL,
} device_power_state_t;

typedef enum {
    DEVICE_LOW_BATTERY_NORMAL = 0,
    DEVICE_LOW_BATTERY_WARNING,
    DEVICE_LOW_BATTERY_GRACE,
    DEVICE_LOW_BATTERY_SHUTDOWN,
} device_low_battery_phase_t;

typedef enum {
    DEVICE_BATTERY_SOC_INVALID = 0,
    DEVICE_BATTERY_SOC_VOLTAGE_ONLY,
    DEVICE_BATTERY_SOC_COULOMB_TRACKED,
    DEVICE_BATTERY_SOC_FULL_ANCHORED,
} device_battery_soc_quality_t;

esp_err_t device_status_set_battery_soc_quality(device_battery_soc_quality_t quality);

esp_err_t device_status_set_power_state(
    device_power_state_t state,
    bool battery_full,
    device_low_battery_phase_t low_battery,
    uint32_t shutdown_in_s
);

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
