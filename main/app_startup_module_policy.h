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

typedef struct {
    hw_module_state_t state;
    const char *reason;
} app_startup_barometer_verdict_t;

/* Boot inventory verdict for the MAIN barometer. Its polling only starts with the workflow's sensor check, so a
 * status that says "not present" before polling was requested is "unknown", not "absent". */
app_startup_barometer_verdict_t app_startup_barometer_inventory(
    bool polling_requested,
    bool present,
    bool valid);

app_startup_module_policy_t app_startup_module_policy_from_inventory(
    const hw_module_info_t *modules,
    size_t count);
