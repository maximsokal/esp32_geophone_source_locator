#include "hw_inventory_status_fake.h"

#include <string.h>

static hw_module_info_t s_modules[HW_MODULE_COUNT] = {
    { HW_MODULE_SD_CARD, HW_STATE_UNKNOWN, HW_REQ_REQUIRED, "" },
    { HW_MODULE_AD7606, HW_STATE_UNKNOWN, HW_REQ_REQUIRED, "" },
    { HW_MODULE_SENSOR_MUX, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
    { HW_MODULE_MAIN_XIAO, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
    { HW_MODULE_BAROMETER, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
    { HW_MODULE_POWER_MONITOR, HW_STATE_UNKNOWN, HW_REQ_OPTIONAL, "" },
};

void hw_inventory_status_fake_set(const hw_module_info_t *modules, size_t count)
{
    if (modules == NULL || count != (size_t)HW_MODULE_COUNT) return;
    memcpy(s_modules, modules, sizeof(s_modules));
}

size_t hw_inventory_snapshot(hw_module_info_t *out, size_t cap)
{
    if (out == NULL) return 0U;
    const size_t module_count = (size_t)HW_MODULE_COUNT;
    const size_t count = cap < module_count ? cap : module_count;
    memcpy(out, s_modules, count * sizeof(*out));
    return count;
}

const char *hw_inventory_module_name(hw_module_id_t id)
{
    static const char *const names[HW_MODULE_COUNT] = {
        "sd_card", "ad7606", "sensor_mux", "main_xiao", "barometer", "power_monitor",
    };
    return (unsigned)id < (unsigned)HW_MODULE_COUNT ? names[(unsigned)id] : "unknown";
}

const char *hw_inventory_state_name(hw_module_state_t state)
{
    switch (state) {
        case HW_STATE_PRESENT: return "present";
        case HW_STATE_ABSENT: return "absent";
        case HW_STATE_FAULT: return "fault";
        case HW_STATE_DEGRADED: return "degraded";
        case HW_STATE_UNKNOWN:
        default: return "unknown";
    }
}
