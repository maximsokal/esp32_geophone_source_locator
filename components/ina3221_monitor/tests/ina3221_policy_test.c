#include "ina3221_policy.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


static void assert_near(
    float actual,
    float expected,
    float epsilon
)
{
    float difference =
        actual - expected;

    if (difference < 0.0f) {
        difference =
            -difference;
    }

    assert(
        difference <=
        epsilon
    );
}


static ina3221_channel_sample_t channel(
    float bus_voltage_v,
    float shunt_voltage_v,
    float current_a,
    bool valid
)
{
    return
        (ina3221_channel_sample_t) {
            .valid = valid,
            .bus_voltage_v = bus_voltage_v,
            .shunt_voltage_v = shunt_voltage_v,
            .current_a = current_a,
        };
}


static void test_register_conversion(void)
{
    assert_near(
        ina3221_policy_bus_raw_to_volts(
            (uint16_t)(625u << 3)
        ),
        5.0f,
        0.0001f
    );

    assert_near(
        ina3221_policy_shunt_raw_to_volts(
            (uint16_t)(250u << 3)
        ),
        0.010f,
        0.000001f
    );

    assert_near(
        ina3221_policy_shunt_raw_to_volts(
            (uint16_t)(int16_t)(-250 * 8)
        ),
        -0.010f,
        0.000001f
    );

    assert_near(
        ina3221_policy_current_from_shunt(
            0.010f,
            0.1f
        ),
        0.100f,
        0.0001f
    );
}


static void test_battery_source_voltage_and_percent(void)
{
    const ina3221_channel_sample_t battery =
        channel(
            3.89f,
            0.01f,
            0.10f,
            true
        );

    assert_near(
        ina3221_policy_battery_source_voltage(
            &battery
        ),
        3.90f,
        0.0001f
    );

    assert(
        ina3221_policy_battery_present(
            3.90f
        )
    );

    assert(
        !ina3221_policy_battery_present(
            5.00f
        )
    );

    float previous =
        -1.0f;

    for (
        int centivolts = 300;
        centivolts <= 420;
        ++centivolts
    ) {
        const float voltage_v =
            (float)centivolts /
            100.0f;

        const float percent =
            ina3221_policy_battery_percent(
                voltage_v
            );

        assert(
            percent >=
            previous
        );

        previous =
            percent;
    }

    assert_near(
        ina3221_policy_battery_percent(
            3.00f
        ),
        0.0f,
        0.001f
    );

    assert_near(
        ina3221_policy_battery_percent(
            4.20f
        ),
        100.0f,
        0.001f
    );
}


static void test_power_state_policy(void)
{
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT] = {
        channel(3.89f, 0.01f, 0.10f, true),
        channel(5.00f, 0.00f, 0.20f, true),
        channel(0.00f, 0.00f, 0.00f, false),
    };

    /* Unused CH2 must not affect battery/source classification. */
    assert(
        ina3221_policy_classify_instant(
            channels
        ) ==
        INA3221_POWER_STATE_BATTERY
    );

    channels[0] =
        channel(
            3.92f,
            -0.02f,
            -0.20f,
            true
        );

    assert(
        ina3221_policy_classify_instant(
            channels
        ) ==
        INA3221_POWER_STATE_USB_CHARGING
    );

    /*
     * Current bench condition: no 18650 installed, CH0 sits near 5 V from
     * USB-side circuitry, while the ESP32 5 V rail is present.
     */
    channels[0] =
        channel(
            5.00f,
            0.00f,
            0.00f,
            true
        );

    assert(
        ina3221_policy_classify_instant(
            channels
        ) ==
        INA3221_POWER_STATE_USB
    );

    channels[0].valid =
        false;

    assert(
        ina3221_policy_classify_instant(
            channels
        ) ==
        INA3221_POWER_STATE_UNKNOWN
    );
}


static void test_hysteresis(void)
{
    ina3221_power_classifier_t classifier = { 0 };

    assert(
        ina3221_policy_classifier_update(
            &classifier,
            INA3221_POWER_STATE_USB
        ) ==
        INA3221_POWER_STATE_USB
    );

    assert(
        ina3221_policy_classifier_update(
            &classifier,
            INA3221_POWER_STATE_BATTERY
        ) ==
        INA3221_POWER_STATE_USB
    );

    assert(
        ina3221_policy_classifier_update(
            &classifier,
            INA3221_POWER_STATE_BATTERY
        ) ==
        INA3221_POWER_STATE_USB
    );

    assert(
        ina3221_policy_classifier_update(
            &classifier,
            INA3221_POWER_STATE_BATTERY
        ) ==
        INA3221_POWER_STATE_BATTERY
    );
}


static void test_decision_fields(void)
{
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT] = {
        channel(3.92f, -0.02f, -0.20f, true),
        channel(4.98f, 0.01f, 0.10f, true),
        channel(0.00f, 0.00f, 0.00f, false),
    };

    ina3221_power_decision_t decision;

    ina3221_policy_make_decision(
        channels,
        INA3221_POWER_STATE_USB_CHARGING,
        &decision
    );

    assert(decision.battery_present);
    assert(decision.battery_percent_valid);
    assert(decision.usb_present);
    assert(decision.charging);

    assert_near(
        decision.battery_voltage_v,
        3.90f,
        0.0001f
    );

    channels[0] =
        channel(
            5.00f,
            0.00f,
            0.00f,
            true
        );

    ina3221_policy_make_decision(
        channels,
        INA3221_POWER_STATE_USB,
        &decision
    );

    assert(!decision.battery_present);
    assert(!decision.battery_percent_valid);
    assert(decision.usb_present);
    assert(!decision.charging);
    assert_near(decision.battery_voltage_v, 0.0f, 0.0001f);
}


int main(void)
{
    test_register_conversion();
    test_battery_source_voltage_and_percent();
    test_power_state_policy();
    test_hysteresis();
    test_decision_fields();

    puts("INA3221 policy tests passed");

    return 0;
}
