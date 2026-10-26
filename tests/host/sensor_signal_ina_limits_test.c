#define main ina_limits_baseline_main
#include "../../components/ina3221_monitor/tests/ina3221_limits_test.c"
#undef main

#include <float.h>
#include <stdio.h>

static void test_extreme_limit_commands(void)
{
    assert(ina3221_limits_register_value(FLT_MAX, 0.1f) == 0x7FF8U);
    assert(ina3221_limits_register_value(-FLT_MAX, 0.1f) == 0x8000U);
    assert(ina3221_limits_register_value(1.0f, INFINITY) == INA3221_LIMITS_REG_DISABLED);
    assert(ina3221_limits_register_value(1.0f, NAN) == INA3221_LIMITS_REG_DISABLED);
    for (int counts = -4096; counts <= 4095; ++counts) {
        const float current = (float)counts * 0.0004f;
        assert(ina3221_limits_register_value(current, 0.1f) ==
            (uint16_t)(int16_t)(counts * 8));
    }
    puts("SIGNAL_CASE INA.LIMIT_WORDS PASS codes=8192");
}

static void test_invalid_current_cannot_latch_overcurrent(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    const float bad[] = {NAN, INFINITY, -INFINITY};
    for (size_t v = 0U; v < 3U; ++v) {
        ina3221_overcurrent_tracker_t tracker = {0};
        ina3221_channel_sample_t readings[3] = {0};
        readings[0].valid = true;
        readings[0].current_a = bad[v];
        assert(!ina3221_overcurrent_update(&tracker, &limits, readings));
        assert(!ina3221_overcurrent_update(&tracker, &limits, readings));
        assert(!tracker.channels[0].latched);
    }
    puts("SIGNAL_CASE INA.INVALID_ALERT PASS");
}

int main(void)
{
    assert(ina_limits_baseline_main() == 0);
    test_extreme_limit_commands();
    test_invalid_current_cannot_latch_overcurrent();
    return 0;
}
