#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hw_inventory.h"


static const hw_module_requirement_t s_default_requirements[HW_MODULE_COUNT] = {
    HW_REQ_REQUIRED,
    HW_REQ_REQUIRED,
    HW_REQ_OPTIONAL,
    HW_REQ_OPTIONAL,
    HW_REQ_OPTIONAL,
    HW_REQ_OPTIONAL,
};


static hw_module_info_t get_module(hw_module_id_t id)
{
    hw_module_info_t module;
    assert(hw_inventory_get(id, &module));
    return module;
}


static void assert_all_defaults(void)
{
    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        const hw_module_info_t module = get_module((hw_module_id_t)i);
        assert(module.id == (hw_module_id_t)i);
        assert(module.state == HW_STATE_UNKNOWN);
        assert(module.requirement == s_default_requirements[i]);
        assert(module.reason[0] == '\0');
    }
}


static void test_defaults_are_available_before_init(void)
{
    assert_all_defaults();
}


static void test_report_get_round_trip_and_null_reason(void)
{
    hw_inventory_report(HW_MODULE_SENSOR_MUX, HW_STATE_DEGRADED, "mux responded slowly");

    hw_module_info_t module = get_module(HW_MODULE_SENSOR_MUX);
    assert(module.id == HW_MODULE_SENSOR_MUX);
    assert(module.state == HW_STATE_DEGRADED);
    assert(module.requirement == HW_REQ_OPTIONAL);
    assert(strcmp(module.reason, "mux responded slowly") == 0);

    hw_inventory_report(HW_MODULE_AD7606, HW_STATE_PRESENT, NULL);
    module = get_module(HW_MODULE_AD7606);
    assert(module.state == HW_STATE_PRESENT);
    assert(module.requirement == HW_REQ_REQUIRED);
    assert(module.reason[0] == '\0');
}


static void test_reason_truncates_and_is_terminated(void)
{
    char long_reason[96];
    memset(long_reason, 'x', sizeof(long_reason) - 1U);
    long_reason[sizeof(long_reason) - 1U] = '\0';

    hw_inventory_report(HW_MODULE_BAROMETER, HW_STATE_FAULT, long_reason);
    const hw_module_info_t module = get_module(HW_MODULE_BAROMETER);

    assert(strlen(module.reason) == HW_INVENTORY_REASON_LEN - 1U);
    assert(module.reason[HW_INVENTORY_REASON_LEN - 1U] == '\0');
    for (size_t i = 0U; i < HW_INVENTORY_REASON_LEN - 1U; ++i) {
        assert(module.reason[i] == 'x');
    }
}


static void test_invalid_ids_and_states_are_ignored(void)
{
    hw_inventory_init();
    hw_inventory_report(HW_MODULE_SD_CARD, HW_STATE_PRESENT, "known good");
    const hw_module_info_t before = get_module(HW_MODULE_SD_CARD);

    hw_inventory_report((hw_module_id_t)-1, HW_STATE_ABSENT, "bad id");
    hw_inventory_report((hw_module_id_t)HW_MODULE_COUNT, HW_STATE_FAULT, "bad id");
    hw_inventory_report(HW_MODULE_SD_CARD, (hw_module_state_t)-1, "bad state");
    hw_inventory_report(
        HW_MODULE_SD_CARD,
        (hw_module_state_t)(HW_STATE_DEGRADED + 1),
        "bad state"
    );

    const hw_module_info_t after = get_module(HW_MODULE_SD_CARD);
    assert(after.state == before.state);
    assert(after.requirement == before.requirement);
    assert(strcmp(after.reason, before.reason) == 0);
    assert(!hw_inventory_get((hw_module_id_t)-1, &(hw_module_info_t){0}));
    assert(!hw_inventory_get((hw_module_id_t)HW_MODULE_COUNT, &(hw_module_info_t){0}));
    assert(!hw_inventory_get(HW_MODULE_SD_CARD, NULL));
}


static void test_snapshot_caps_and_order(void)
{
    hw_inventory_init();
    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        hw_inventory_report(
            (hw_module_id_t)i,
            (hw_module_state_t)(i % 5U),
            "snapshot"
        );
    }

    hw_module_info_t snapshot[HW_MODULE_COUNT];
    assert(hw_inventory_snapshot(snapshot, 0U) == 0U);
    assert(hw_inventory_snapshot(NULL, 0U) == 0U);
    assert(hw_inventory_snapshot(NULL, (size_t)HW_MODULE_COUNT) == 0U);
    assert(hw_inventory_snapshot(snapshot, 3U) == 3U);
    for (size_t i = 0U; i < 3U; ++i) {
        assert(snapshot[i].id == (hw_module_id_t)i);
        assert(snapshot[i].reason[0] == 's');
    }

    const size_t larger_cap = (size_t)HW_MODULE_COUNT + 4U;
    assert(hw_inventory_snapshot(snapshot, larger_cap) == (size_t)HW_MODULE_COUNT);
    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        assert(snapshot[i].id == (hw_module_id_t)i);
        assert(snapshot[i].state == (hw_module_state_t)(i % 5U));
    }
}


static void test_probe_error_classification(void)
{
    assert(hw_inventory_classify_probe_error(ESP_OK) == HW_STATE_PRESENT);
    assert(hw_inventory_classify_probe_error(ESP_ERR_NOT_FOUND) == HW_STATE_ABSENT);
    assert(hw_inventory_classify_probe_error(ESP_ERR_TIMEOUT) == HW_STATE_FAULT);
    assert(hw_inventory_classify_probe_error(ESP_ERR_INVALID_STATE) == HW_STATE_FAULT);
    assert(hw_inventory_classify_probe_error(ESP_ERR_INVALID_RESPONSE) == HW_STATE_FAULT);
    assert(hw_inventory_classify_probe_error(ESP_FAIL) == HW_STATE_FAULT);
}


static void assert_required_state(hw_module_id_t id, hw_module_state_t state, bool expected_missing)
{
    hw_inventory_init();
    hw_inventory_report(id, state, "probe result");

    hw_module_id_t first_missing = HW_MODULE_POWER_MONITOR;
    const bool missing = hw_inventory_required_missing(&first_missing);
    assert(missing == expected_missing);
    if (expected_missing) {
        assert(first_missing == id);
    }
}


static void test_required_missing_for_states_and_requirements(void)
{
    static const hw_module_state_t states[] = {
        HW_STATE_UNKNOWN,
        HW_STATE_PRESENT,
        HW_STATE_ABSENT,
        HW_STATE_FAULT,
        HW_STATE_DEGRADED,
    };

    hw_inventory_init();
    assert(!hw_inventory_required_missing(NULL));

    for (size_t i = 0U; i < sizeof(states) / sizeof(states[0]); ++i) {
        const bool should_be_missing = states[i] == HW_STATE_ABSENT || states[i] == HW_STATE_FAULT;
        assert_required_state(HW_MODULE_SD_CARD, states[i], should_be_missing);
        assert_required_state(HW_MODULE_AD7606, states[i], should_be_missing);

        hw_inventory_init();
        hw_inventory_report(HW_MODULE_SENSOR_MUX, states[i], "optional probe result");
        assert(!hw_inventory_required_missing(NULL));
    }

    hw_inventory_init();
    hw_inventory_report(HW_MODULE_AD7606, HW_STATE_FAULT, "later required module");
    hw_inventory_report(HW_MODULE_SD_CARD, HW_STATE_ABSENT, "first required module");
    hw_module_id_t first_missing = HW_MODULE_POWER_MONITOR;
    assert(hw_inventory_required_missing(&first_missing));
    assert(first_missing == HW_MODULE_SD_CARD);

    hw_inventory_init();
    hw_inventory_report(HW_MODULE_AD7606, HW_STATE_ABSENT, "required module");
    assert(hw_inventory_required_missing(NULL));
}


static void test_init_resets_states_and_reasons(void)
{
    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        hw_inventory_report((hw_module_id_t)i, HW_STATE_FAULT, "reset me");
    }

    hw_inventory_init();
    hw_inventory_init();
    assert_all_defaults();
}


static void test_names_and_unknown_values(void)
{
    static const char *const module_names[HW_MODULE_COUNT] = {
        "sd_card",
        "ad7606",
        "sensor_mux",
        "main_xiao",
        "barometer",
        "power_monitor",
    };
    static const char *const state_names[] = {
        "unknown",
        "present",
        "absent",
        "fault",
        "degraded",
    };

    for (size_t i = 0U; i < (size_t)HW_MODULE_COUNT; ++i) {
        assert(strcmp(hw_inventory_module_name((hw_module_id_t)i), module_names[i]) == 0);
    }
    assert(strcmp(hw_inventory_module_name((hw_module_id_t)-1), "unknown") == 0);
    assert(strcmp(hw_inventory_module_name((hw_module_id_t)HW_MODULE_COUNT), "unknown") == 0);

    for (size_t i = 0U; i < sizeof(state_names) / sizeof(state_names[0]); ++i) {
        assert(strcmp(hw_inventory_state_name((hw_module_state_t)i), state_names[i]) == 0);
    }
    assert(strcmp(hw_inventory_state_name((hw_module_state_t)-1), "unknown") == 0);
    assert(
        strcmp(
            hw_inventory_state_name((hw_module_state_t)(HW_STATE_DEGRADED + 1)),
            "unknown"
        ) == 0
    );
}


int main(void)
{
    test_defaults_are_available_before_init();
    test_report_get_round_trip_and_null_reason();
    test_reason_truncates_and_is_terminated();
    test_invalid_ids_and_states_are_ignored();
    test_snapshot_caps_and_order();
    test_probe_error_classification();
    test_required_missing_for_states_and_requirements();
    test_init_resets_states_and_reasons();
    test_names_and_unknown_values();
    puts("hardware inventory tests passed");
    return 0;
}
