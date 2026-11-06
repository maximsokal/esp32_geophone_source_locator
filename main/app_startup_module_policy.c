#include "app_startup_module_policy.h"

static bool app_startup_module_is_present(
    const hw_module_info_t *modules,
    size_t count,
    hw_module_id_t id)
{
    if (modules == NULL) {
        return false;
    }

    for (size_t index = 0U; index < count; ++index) {
        if (modules[index].id == id) {
            return modules[index].state == HW_STATE_PRESENT;
        }
    }

    return false;
}

app_startup_barometer_verdict_t app_startup_barometer_inventory(
    bool polling_requested,
    bool present,
    bool valid)
{
    if (present && valid) {
        return (app_startup_barometer_verdict_t){HW_STATE_PRESENT, "sensor ok"};
    }
    if (present) {
        return (app_startup_barometer_verdict_t){HW_STATE_FAULT, "sensor invalid"};
    }
    if (!polling_requested) {
        return (app_startup_barometer_verdict_t){HW_STATE_UNKNOWN, "not polled yet"};
    }
    return (app_startup_barometer_verdict_t){HW_STATE_ABSENT, "no response"};
}

app_startup_module_policy_t app_startup_module_policy_from_inventory(
    const hw_module_info_t *modules,
    size_t count)
{
    return (app_startup_module_policy_t) {
        .acquisition_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_AD7606),
        .sensor_mux_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_SENSOR_MUX),
        .main_xiao_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_MAIN_XIAO),
        .barometer_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_BAROMETER),
        .power_monitor_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_POWER_MONITOR),
        .storage_allowed = app_startup_module_is_present(
            modules, count, HW_MODULE_SD_CARD),
    };
}
