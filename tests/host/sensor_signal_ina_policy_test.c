#define main ina_policy_baseline_main
#include "../../components/ina3221_monitor/tests/ina3221_policy_test.c"
#undef main

#include <math.h>

static void test_all_ina_register_words(void)
{
    /* TI SBOS576C tables 7-8/7-10: both registers have a signed 13-bit value. */
    for (uint32_t word = 0U; word <= UINT16_MAX; ++word) {
        const int32_t counts = word < 0x8000U ? (int32_t)(word / 8U)
            : (int32_t)(word / 8U) - 8192;
        assert_near(ina3221_policy_bus_raw_to_volts((uint16_t)word),
            (float)counts * 0.008f, 0.00001f);
        assert_near(ina3221_policy_shunt_raw_to_volts((uint16_t)word),
            (float)counts * 0.000040f, 0.0000001f);
        const float shunt = ina3221_policy_shunt_raw_to_volts((uint16_t)word);
        assert_near(ina3221_policy_current_for_channel(0U, shunt, 0.1f),
            shunt / 0.1f, 0.000001f);
        for (size_t rail = 1U; rail < 3U; ++rail) {
            assert_near(ina3221_policy_current_for_channel(rail, shunt, 0.1f),
                -shunt / 0.1f, 0.000001f);
        }
    }
    puts("SIGNAL_CASE INA.WORDS PASS words=65536 channels=3");
}

static void test_nonfinite_ina_measurements(void)
{
    const float bad[] = {NAN, INFINITY, -INFINITY};
    for (size_t value = 0U; value < 3U; ++value) {
        assert(ina3221_policy_current_from_shunt(bad[value], 0.1f) == 0.0f);
        assert(ina3221_policy_current_from_shunt(0.1f, bad[value]) == 0.0f);
        assert(ina3221_policy_battery_percent(bad[value]) == 0.0f);
        for (unsigned field = 0U; field < 3U; ++field) {
            ina3221_channel_sample_t samples[3] = {
                channel(3.8f, 0.0f, 0.1f, true),
                channel(5.0f, 0.0f, 0.0f, true),
                channel(5.0f, 0.0f, 0.0f, true),
            };
            if (field == 0U) samples[0].bus_voltage_v = bad[value];
            if (field == 1U) samples[0].shunt_voltage_v = bad[value];
            if (field == 2U) samples[0].current_a = bad[value];
            assert(ina3221_policy_classify_instant(samples) == INA3221_POWER_STATE_UNKNOWN);
            ina3221_power_decision_t result;
            ina3221_policy_make_decision(samples, INA3221_POWER_STATE_BATTERY, &result);
            assert(!result.battery_present && !result.battery_percent_valid);
            assert(isfinite(result.battery_current_a) && isfinite(result.battery_voltage_v));
        }
    }
    ina3221_power_classifier_t classifier = {0};
    assert(ina3221_policy_classifier_update(&classifier, (ina3221_power_state_t)-1) ==
        INA3221_POWER_STATE_UNKNOWN);
    assert(!classifier.initialized);
    puts("SIGNAL_CASE INA.INVALID_NUMBERS PASS");
}

int main(void)
{
    assert(ina_policy_baseline_main() == 0);
    test_all_ina_register_words();
    test_nonfinite_ina_measurements();
    return 0;
}
