#include "hw_inventory.h"

#include <string.h>

#include "esp_log.h"

#ifdef HW_INVENTORY_HOST_TEST
#include <stdatomic.h>
#else
#include "freertos/FreeRTOS.h"
#endif


#define TAG "HW_INVENTORY"


static hw_module_info_t s_modules[HW_MODULE_COUNT] = {
    [HW_MODULE_SD_CARD] = {
        .id = HW_MODULE_SD_CARD,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_REQUIRED,
        .reason = "",
    },
    [HW_MODULE_AD7606] = {
        .id = HW_MODULE_AD7606,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_REQUIRED,
        .reason = "",
    },
    [HW_MODULE_SENSOR_MUX] = {
        .id = HW_MODULE_SENSOR_MUX,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_OPTIONAL,
        .reason = "",
    },
    [HW_MODULE_MAIN_XIAO] = {
        .id = HW_MODULE_MAIN_XIAO,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_OPTIONAL,
        .reason = "",
    },
    [HW_MODULE_BAROMETER] = {
        .id = HW_MODULE_BAROMETER,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_OPTIONAL,
        .reason = "",
    },
    [HW_MODULE_POWER_MONITOR] = {
        .id = HW_MODULE_POWER_MONITOR,
        .state = HW_STATE_UNKNOWN,
        .requirement = HW_REQ_OPTIONAL,
        .reason = "",
    },
};

#ifdef HW_INVENTORY_HOST_TEST
/* Host tests have no FreeRTOS; a spinlock keeps the same critical sections. */
typedef atomic_flag hw_inventory_lock_t;
#define HW_INVENTORY_LOCK_INITIALIZER ATOMIC_FLAG_INIT

static void hw_inventory_lock(hw_inventory_lock_t *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
    }
}

static void hw_inventory_unlock(hw_inventory_lock_t *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}
#else
typedef portMUX_TYPE hw_inventory_lock_t;
#define HW_INVENTORY_LOCK_INITIALIZER portMUX_INITIALIZER_UNLOCKED

static void hw_inventory_lock(hw_inventory_lock_t *lock)
{
    portENTER_CRITICAL(lock);
}

static void hw_inventory_unlock(hw_inventory_lock_t *lock)
{
    portEXIT_CRITICAL(lock);
}
#endif

static hw_inventory_lock_t s_lock = HW_INVENTORY_LOCK_INITIALIZER;


static bool hw_module_id_valid(hw_module_id_t id)
{
    return (int)id >= 0 && id < HW_MODULE_COUNT;
}


static bool hw_module_state_valid(hw_module_state_t state)
{
    return (int)state >= HW_STATE_UNKNOWN && state <= HW_STATE_DEGRADED;
}


void hw_inventory_init(void)
{
    hw_inventory_lock(&s_lock);

    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        s_modules[i].state = HW_STATE_UNKNOWN;
        memset(s_modules[i].reason, 0, sizeof(s_modules[i].reason));
    }

    hw_inventory_unlock(&s_lock);
}


void hw_inventory_report(hw_module_id_t id, hw_module_state_t state, const char *reason)
{
    if (!hw_module_id_valid(id) || !hw_module_state_valid(state)) {
        return;
    }

    hw_inventory_lock(&s_lock);

    hw_module_info_t *module = &s_modules[id];
    module->state = state;
    memset(module->reason, 0, sizeof(module->reason));

    if (reason != NULL) {
        size_t length = 0U;
        while (length < sizeof(module->reason) - 1U && reason[length] != '\0') {
            ++length;
        }
        memcpy(module->reason, reason, length);
    }

    hw_inventory_unlock(&s_lock);
}


bool hw_inventory_get(hw_module_id_t id, hw_module_info_t *out)
{
    if (!hw_module_id_valid(id) || out == NULL) {
        return false;
    }

    hw_inventory_lock(&s_lock);
    *out = s_modules[id];
    hw_inventory_unlock(&s_lock);

    return true;
}


size_t hw_inventory_snapshot(hw_module_info_t *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return 0U;
    }

    const size_t count = cap < (size_t)HW_MODULE_COUNT ? cap : (size_t)HW_MODULE_COUNT;

    hw_inventory_lock(&s_lock);
    for (size_t i = 0U; i < count; ++i) {
        out[i] = s_modules[i];
    }
    hw_inventory_unlock(&s_lock);

    return count;
}


hw_module_state_t hw_inventory_classify_probe_error(esp_err_t err)
{
    if (err == ESP_OK) {
        return HW_STATE_PRESENT;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        return HW_STATE_ABSENT;
    }

    return HW_STATE_FAULT;
}


bool hw_inventory_required_missing(hw_module_id_t *first_missing)
{
    bool missing = false;

    hw_inventory_lock(&s_lock);

    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        const hw_module_info_t *module = &s_modules[i];
        if (module->requirement == HW_REQ_REQUIRED &&
            (module->state == HW_STATE_ABSENT || module->state == HW_STATE_FAULT)) {
            if (first_missing != NULL) {
                *first_missing = module->id;
            }
            missing = true;
            break;
        }
    }

    hw_inventory_unlock(&s_lock);

    return missing;
}


const char *hw_inventory_module_name(hw_module_id_t id)
{
    static const char *const names[HW_MODULE_COUNT] = {
        "sd_card",
        "ad7606",
        "sensor_mux",
        "main_xiao",
        "barometer",
        "power_monitor",
    };

    return hw_module_id_valid(id) ? names[id] : "unknown";
}


const char *hw_inventory_state_name(hw_module_state_t state)
{
    static const char *const names[] = {
        "unknown",
        "present",
        "absent",
        "fault",
        "degraded",
    };

    return hw_module_state_valid(state) ? names[state] : "unknown";
}


void hw_inventory_log_summary(void)
{
    hw_module_info_t snapshot[HW_MODULE_COUNT];
    const size_t count = hw_inventory_snapshot(snapshot, (size_t)HW_MODULE_COUNT);

    for (size_t i = 0U; i < count; ++i) {
        ESP_LOGI(
            TAG,
            "HW_MODULE id=%s state=%s req=%s reason=%s",
            hw_inventory_module_name(snapshot[i].id),
            hw_inventory_state_name(snapshot[i].state),
            snapshot[i].requirement == HW_REQ_REQUIRED ? "required" : "optional",
            snapshot[i].reason
        );
    }
}
