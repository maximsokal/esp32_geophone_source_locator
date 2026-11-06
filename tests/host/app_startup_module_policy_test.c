#include "app_startup_module_policy.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#define MODULE_STATE_COUNT 5U
#define MODULE_COMBINATION_COUNT \
    (MODULE_STATE_COUNT * MODULE_STATE_COUNT * MODULE_STATE_COUNT * \
     MODULE_STATE_COUNT * MODULE_STATE_COUNT * MODULE_STATE_COUNT)

static hw_module_state_t s_states[HW_MODULE_COUNT];

static void assert_policy_for_combination(void)
{
    hw_inventory_init();
    for (size_t index = 0U; index < HW_MODULE_COUNT; ++index) {
        hw_inventory_report(
            (hw_module_id_t)index,
            s_states[index],
            "combination test");
    }

    hw_module_info_t modules[HW_MODULE_COUNT];
    assert(hw_inventory_snapshot(modules, HW_MODULE_COUNT) == HW_MODULE_COUNT);

    const app_startup_module_policy_t policy =
        app_startup_module_policy_from_inventory(modules, HW_MODULE_COUNT);
    assert(policy.storage_allowed == (s_states[HW_MODULE_SD_CARD] == HW_STATE_PRESENT));
    assert(policy.acquisition_allowed == (s_states[HW_MODULE_AD7606] == HW_STATE_PRESENT));
    assert(policy.sensor_mux_allowed == (s_states[HW_MODULE_SENSOR_MUX] == HW_STATE_PRESENT));
    assert(policy.main_xiao_allowed == (s_states[HW_MODULE_MAIN_XIAO] == HW_STATE_PRESENT));
    assert(policy.barometer_allowed == (s_states[HW_MODULE_BAROMETER] == HW_STATE_PRESENT));
    assert(policy.power_monitor_allowed == (s_states[HW_MODULE_POWER_MONITOR] == HW_STATE_PRESENT));

    hw_module_id_t first_missing = HW_MODULE_COUNT;
    const bool required_missing =
        s_states[HW_MODULE_SD_CARD] == HW_STATE_ABSENT ||
        s_states[HW_MODULE_SD_CARD] == HW_STATE_FAULT ||
        s_states[HW_MODULE_AD7606] == HW_STATE_ABSENT ||
        s_states[HW_MODULE_AD7606] == HW_STATE_FAULT;
    assert(hw_inventory_required_missing(&first_missing) == required_missing);
    if (required_missing) {
        const hw_module_id_t expected_first_missing =
            s_states[HW_MODULE_SD_CARD] == HW_STATE_ABSENT ||
            s_states[HW_MODULE_SD_CARD] == HW_STATE_FAULT
                ? HW_MODULE_SD_CARD
                : HW_MODULE_AD7606;
        assert(first_missing == expected_first_missing);
    }
}

static void assert_every_module_state_combination(void)
{
    for (uint32_t combination = 0U;
         combination < MODULE_COMBINATION_COUNT;
         ++combination) {
        uint32_t encoded = combination;
        for (size_t index = 0U; index < HW_MODULE_COUNT; ++index) {
            s_states[index] = (hw_module_state_t)(encoded % MODULE_STATE_COUNT);
            encoded /= MODULE_STATE_COUNT;
        }
        assert_policy_for_combination();
    }
}

static void assert_incomplete_input_disallows_services(void)
{
    const app_startup_module_policy_t empty =
        app_startup_module_policy_from_inventory(NULL, HW_MODULE_COUNT);
    assert(!empty.acquisition_allowed);
    assert(!empty.sensor_mux_allowed);
    assert(!empty.main_xiao_allowed);
    assert(!empty.barometer_allowed);
    assert(!empty.power_monitor_allowed);
    assert(!empty.storage_allowed);

    hw_module_info_t one_module = {
        .id = HW_MODULE_AD7606,
        .state = HW_STATE_PRESENT,
    };
    const app_startup_module_policy_t partial =
        app_startup_module_policy_from_inventory(&one_module, 1U);
    assert(partial.acquisition_allowed);
    assert(!partial.sensor_mux_allowed);
    assert(!partial.main_xiao_allowed);
    assert(!partial.barometer_allowed);
    assert(!partial.power_monitor_allowed);
    assert(!partial.storage_allowed);
}

/* 2026-11-05 hardware log: the barometer was reported "absent: no response" 7 s after boot although the BMP280 answered
 * at 12.5 s. Polling starts with the sensor check, so before that no answer is not an absence. */
static void assert_barometer_inventory_distinguishes_unpolled_from_absent(void)
{
    app_startup_barometer_verdict_t verdict = app_startup_barometer_inventory(false, false, false);
    assert(verdict.state == HW_STATE_UNKNOWN);
    assert(verdict.reason != NULL && verdict.reason[0] != '\0');

    verdict = app_startup_barometer_inventory(true, false, false);
    assert(verdict.state == HW_STATE_ABSENT);                 /* polled and silent: really absent */

    verdict = app_startup_barometer_inventory(true, true, true);
    assert(verdict.state == HW_STATE_PRESENT);
    verdict = app_startup_barometer_inventory(true, true, false);
    assert(verdict.state == HW_STATE_FAULT);
    /* A sensor that already answered is reported as such whether or not the request flag is still visible. */
    verdict = app_startup_barometer_inventory(false, true, true);
    assert(verdict.state == HW_STATE_PRESENT);
    verdict = app_startup_barometer_inventory(false, true, false);
    assert(verdict.state == HW_STATE_FAULT);
}

int main(void)
{
    assert_barometer_inventory_distinguishes_unpolled_from_absent();
    assert_every_module_state_combination();
    assert_incomplete_input_disallows_services();
    return 0;
}
