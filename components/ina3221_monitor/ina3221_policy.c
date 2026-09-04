#include "ina3221_policy.h"

#include <stddef.h>


#define INA3221_BUS_LSB_V              0.008f
#define INA3221_SHUNT_LSB_V            0.000040f

#define INA3221_BATTERY_MIN_PRESENT_V  2.50f
#define INA3221_BATTERY_MAX_PRESENT_V  4.35f

#define INA3221_ESP32_RAIL_PRESENT_V   4.00f
#define INA3221_CURRENT_ACTIVE_A       0.030f


typedef struct
{
    float voltage_v;
    float percent;

} battery_curve_point_t;


float ina3221_policy_bus_raw_to_volts(
    uint16_t raw
)
{
    const uint16_t counts =
        raw >> 3;

    return
        (float)counts *
        INA3221_BUS_LSB_V;
}


float ina3221_policy_shunt_raw_to_volts(
    uint16_t raw
)
{
    /*
     * INA3221 stores the signed 13-bit shunt value in bits 15..3.
     * Mask first so dividing by eight is exact for negative values too and
     * does not depend on implementation-defined signed right shift behavior.
     */
    const int16_t aligned =
        (int16_t)(raw & 0xFFF8u);

    const int16_t counts =
        (int16_t)(aligned / 8);

    return
        (float)counts *
        INA3221_SHUNT_LSB_V;
}


float ina3221_policy_current_from_shunt(
    float shunt_voltage_v,
    float shunt_resistance_ohm
)
{
    if (
        shunt_resistance_ohm <=
        0.0f
    ) {
        return 0.0f;
    }

    return
        shunt_voltage_v /
        shunt_resistance_ohm;
}


float ina3221_policy_battery_source_voltage(
    const ina3221_channel_sample_t *battery
)
{
    if (
        battery == NULL ||
        !battery->valid
    ) {
        return 0.0f;
    }

    /*
     * Bus voltage is measured at IN- (load side). With VI+ on the battery
     * source side, add the signed shunt drop to recover source voltage.
     */
    return
        battery->bus_voltage_v +
        battery->shunt_voltage_v;
}


bool ina3221_policy_battery_present(
    float battery_voltage_v
)
{
    return
        battery_voltage_v >=
            INA3221_BATTERY_MIN_PRESENT_V &&
        battery_voltage_v <=
            INA3221_BATTERY_MAX_PRESENT_V;
}


float ina3221_policy_battery_percent(
    float battery_voltage_v
)
{
    /*
     * Approximate 1S Li-ion voltage-to-SoC curve for the future TFT icon.
     * It is intentionally labelled an estimate: load/charge current shifts
     * terminal voltage and this is not coulomb counting.
     */
    static const battery_curve_point_t curve[] = {
        { 3.00f,   0.0f },
        { 3.30f,   2.0f },
        { 3.50f,   7.0f },
        { 3.60f,  15.0f },
        { 3.70f,  35.0f },
        { 3.80f,  55.0f },
        { 3.90f,  70.0f },
        { 4.00f,  80.0f },
        { 4.10f,  90.0f },
        { 4.20f, 100.0f },
    };

    if (
        battery_voltage_v <=
        curve[0].voltage_v
    ) {
        return 0.0f;
    }

    const size_t count =
        sizeof(curve) /
        sizeof(curve[0]);

    if (
        battery_voltage_v >=
        curve[count - 1].voltage_v
    ) {
        return 100.0f;
    }

    for (
        size_t i = 1;
        i < count;
        ++i
    ) {
        if (
            battery_voltage_v <=
            curve[i].voltage_v
        ) {
            const float x0 =
                curve[i - 1].voltage_v;

            const float x1 =
                curve[i].voltage_v;

            const float y0 =
                curve[i - 1].percent;

            const float y1 =
                curve[i].percent;

            const float t =
                (battery_voltage_v - x0) /
                (x1 - x0);

            return
                y0 +
                (y1 - y0) * t;
        }
    }

    return 100.0f;
}


ina3221_power_state_t ina3221_policy_classify_instant(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT]
)
{
    if (
        channels == NULL ||
        !channels[0].valid
    ) {
        return INA3221_POWER_STATE_UNKNOWN;
    }

    const float battery_voltage_v =
        ina3221_policy_battery_source_voltage(
            &channels[0]
        );

    const bool battery_present =
        ina3221_policy_battery_present(
            battery_voltage_v
        );

    const bool esp32_rail_present =
        channels[1].valid &&
        channels[1].bus_voltage_v >=
            INA3221_ESP32_RAIL_PRESENT_V;

    if (
        battery_present &&
        channels[0].current_a <=
            -INA3221_CURRENT_ACTIVE_A
    ) {
        return
            INA3221_POWER_STATE_USB_CHARGING;
    }

    if (
        battery_present &&
        channels[0].current_a >=
            INA3221_CURRENT_ACTIVE_A
    ) {
        return
            INA3221_POWER_STATE_BATTERY;
    }

    /*
     * CH1 is a 5 V rail, so it exists on USB and on boosted battery power.
     * The battery-current direction therefore has priority above. If the
     * battery is not discharging and the 5 V rail is alive, USB is the best
     * inference available from the approved three-channel wiring.
     */
    if (esp32_rail_present) {
        return
            INA3221_POWER_STATE_USB;
    }

    return
        INA3221_POWER_STATE_UNKNOWN;
}


ina3221_power_state_t ina3221_policy_classifier_update(
    ina3221_power_classifier_t *classifier,
    ina3221_power_state_t instantaneous_state
)
{
    if (classifier == NULL) {
        return
            INA3221_POWER_STATE_UNKNOWN;
    }

    if (!classifier->initialized) {
        classifier->initialized =
            true;

        classifier->stable_state =
            instantaneous_state;

        classifier->candidate_state =
            instantaneous_state;

        classifier->candidate_count =
            0;

        return
            classifier->stable_state;
    }

    if (
        instantaneous_state ==
        classifier->stable_state
    ) {
        classifier->candidate_state =
            instantaneous_state;

        classifier->candidate_count =
            0;

        return
            classifier->stable_state;
    }

    if (
        instantaneous_state !=
        classifier->candidate_state
    ) {
        classifier->candidate_state =
            instantaneous_state;

        classifier->candidate_count =
            1;

        return
            classifier->stable_state;
    }

    if (
        classifier->candidate_count <
        UINT8_MAX
    ) {
        classifier->candidate_count++;
    }

    if (
        classifier->candidate_count >=
        INA3221_POLICY_STABLE_SAMPLES
    ) {
        classifier->stable_state =
            classifier->candidate_state;

        classifier->candidate_count =
            0;
    }

    return
        classifier->stable_state;
}


void ina3221_policy_make_decision(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    ina3221_power_state_t stable_state,
    ina3221_power_decision_t *decision
)
{
    if (decision == NULL) {
        return;
    }

    *decision =
        (ina3221_power_decision_t) {
            .state = stable_state,
        };

    if (
        channels == NULL ||
        !channels[0].valid
    ) {
        return;
    }

    const float battery_voltage_v =
        ina3221_policy_battery_source_voltage(
            &channels[0]
        );

    decision->battery_present =
        ina3221_policy_battery_present(
            battery_voltage_v
        );

    if (decision->battery_present) {
        decision->battery_voltage_v =
            battery_voltage_v;

        decision->battery_current_a =
            channels[0].current_a;

        decision->battery_percent_valid =
            true;

        decision->battery_percent_estimate =
            ina3221_policy_battery_percent(
                battery_voltage_v
            );
    }

    decision->usb_present =
        stable_state ==
            INA3221_POWER_STATE_USB ||
        stable_state ==
            INA3221_POWER_STATE_USB_CHARGING;

    decision->charging =
        stable_state ==
            INA3221_POWER_STATE_USB_CHARGING;
}


const char *ina3221_policy_state_name(
    ina3221_power_state_t state
)
{
    switch (state) {

        case INA3221_POWER_STATE_BATTERY:
            return "BATTERY";

        case INA3221_POWER_STATE_USB:
            return "USB";

        case INA3221_POWER_STATE_USB_CHARGING:
            return "USB_CHARGING";

        case INA3221_POWER_STATE_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}
