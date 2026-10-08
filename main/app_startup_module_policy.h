#pragma once

#include "hw_inventory.h"

typedef struct {
    bool acquisition_allowed;
    bool sensor_mux_allowed;
    bool main_xiao_allowed;
    bool barometer_allowed;
    bool power_monitor_allowed;
    bool storage_allowed;
} app_startup_module_policy_t;

app_startup_module_policy_t app_startup_module_policy_from_inventory(
    const hw_module_info_t *modules,
    size_t count);
