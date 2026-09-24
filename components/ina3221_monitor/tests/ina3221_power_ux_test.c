#include <assert.h>
#include <math.h>
#include <string.h>

#include "ina3221_power_ux.h"

/* MAX-180: user-facing power states, hysteresis and time qualification. */

static ina3221_power_ux_config_t cfg;

static ina3221_power_ux_input_t on_battery(float v)
{
    return (ina3221_power_ux_input_t) {
        .source = INA3221_POWER_STATE_BATTERY, .telemetry_valid = true,
        .battery_present = true, .battery_voltage_v = v,
    };
}

static ina3221_power_ux_state_t feed(ina3221_power_ux_t *ux, ina3221_power_ux_input_t in, int times)
{
    ina3221_power_ux_state_t s = INA3221_POWER_UX_UNKNOWN;
    for (int i = 0; i < times; ++i) {
        s = ina3221_power_ux_update(ux, &cfg, &in);
    }
    return s;
}

static void test_config(void)
{
    ina3221_power_ux_default_config(&cfg);
    assert(ina3221_power_ux_config_valid(&cfg));
    ina3221_power_ux_config_t bad = cfg;
    bad.critical_enter_v = bad.low_enter_v; /* critical must be below low */
    assert(!ina3221_power_ux_config_valid(&bad));
    bad = cfg;
    bad.low_exit_v = bad.low_enter_v; /* no hysteresis */
    assert(!ina3221_power_ux_config_valid(&bad));
    bad = cfg;
    bad.critical_confirm_samples = 1U; /* critical must not be quicker */
    assert(!ina3221_power_ux_config_valid(&bad));
    assert(!ina3221_power_ux_config_valid(NULL));
}

static void test_usb_states(void)
{
    ina3221_power_ux_input_t in = { .source = INA3221_POWER_STATE_USB, .telemetry_valid = true };
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_UNKNOWN, &in) == INA3221_POWER_UX_USB_ONLY_NO_BATTERY);
    in.battery_present = true;
    in.battery_voltage_v = 4.15f;
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_UNKNOWN, &in) == INA3221_POWER_UX_USB_BATTERY_READY);
    in.source = INA3221_POWER_STATE_USB_CHARGING;
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_UNKNOWN, &in) == INA3221_POWER_UX_USB_CHARGING);
    /* With USB, a low battery voltage is not a battery warning. */
    in.battery_voltage_v = 3.2f;
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &in) == INA3221_POWER_UX_USB_CHARGING);
    in.source = INA3221_POWER_STATE_UNKNOWN;
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_UNKNOWN, &in) == INA3221_POWER_UX_UNKNOWN);
    in.telemetry_valid = false;
    in.source = INA3221_POWER_STATE_USB;
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_UNKNOWN, &in) == INA3221_POWER_UX_UNKNOWN);
    assert(ina3221_power_ux_usb_present(INA3221_POWER_UX_USB_BATTERY_READY));
    assert(!ina3221_power_ux_usb_present(INA3221_POWER_UX_BATTERY_LOW));
}

static void test_battery_bands_with_hysteresis(void)
{
    /* Entering: 3.45 V low, 3.30 V critical. */
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.50f}) == INA3221_POWER_UX_BATTERY);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.44f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.29f}) == INA3221_POWER_UX_BATTERY_CRITICAL);
    /* Leaving LOW needs 3.60 V, leaving CRITICAL needs 3.45 V. */
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.55f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.61f}) == INA3221_POWER_UX_BATTERY);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.40f}) == INA3221_POWER_UX_BATTERY_CRITICAL);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.50f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.70f}) == INA3221_POWER_UX_BATTERY);
    /* Battery source without a plausible pack voltage is not a state. */
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, false, 0.0f}) == INA3221_POWER_UX_UNKNOWN);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, NAN}) == INA3221_POWER_UX_UNKNOWN);
}

static void test_time_qualification(void)
{
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);

    /* Booting on a nearly empty pack: never CRITICAL from one sample. */
    assert(ina3221_power_ux_update(&ux, &cfg, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.20f}) == INA3221_POWER_UX_BATTERY);
    assert(feed(&ux, on_battery(3.20f), 3) == INA3221_POWER_UX_BATTERY);
    assert(feed(&ux, on_battery(3.20f), 1) == INA3221_POWER_UX_BATTERY_CRITICAL); /* 5th sample */

    /* A single load spike below critical does not start a shutdown. */
    ina3221_power_ux_reset(&ux);
    assert(feed(&ux, on_battery(3.80f), 3) == INA3221_POWER_UX_BATTERY);
    for (int i = 0; i < 10; ++i) {
        assert(feed(&ux, on_battery(3.25f), 1) == INA3221_POWER_UX_BATTERY);
        assert(feed(&ux, on_battery(3.80f), 1) == INA3221_POWER_UX_BATTERY);
    }

    /* LOW after 3 samples, CRITICAL after 5 more. */
    assert(feed(&ux, on_battery(3.40f), 2) == INA3221_POWER_UX_BATTERY);
    assert(feed(&ux, on_battery(3.40f), 1) == INA3221_POWER_UX_BATTERY_LOW);
    assert(feed(&ux, on_battery(3.28f), 4) == INA3221_POWER_UX_BATTERY_LOW);
    assert(feed(&ux, on_battery(3.28f), 1) == INA3221_POWER_UX_BATTERY_CRITICAL);

    /* Recovery (e.g. load removed) leaves CRITICAL only through the exit band. */
    assert(feed(&ux, on_battery(3.42f), 10) == INA3221_POWER_UX_BATTERY_CRITICAL);
    assert(feed(&ux, on_battery(3.50f), 3) == INA3221_POWER_UX_BATTERY_LOW);

    /* USB plugged in: charging state after 3 samples. */
    ina3221_power_ux_input_t usb = { INA3221_POWER_STATE_USB_CHARGING, true, true, 3.6f };
    assert(feed(&ux, usb, 2) == INA3221_POWER_UX_BATTERY_LOW);
    assert(feed(&ux, usb, 1) == INA3221_POWER_UX_USB_CHARGING);

    /* Flicker between two other states never confirms either. */
    ina3221_power_ux_input_t ready = { INA3221_POWER_STATE_USB, true, true, 4.15f };
    ina3221_power_ux_input_t nobat = { INA3221_POWER_STATE_USB, true, false, 0.0f };
    for (int i = 0; i < 6; ++i) {
        assert(feed(&ux, ready, 1) == INA3221_POWER_UX_USB_CHARGING);
        assert(feed(&ux, nobat, 1) == INA3221_POWER_UX_USB_CHARGING);
    }
}

static void test_names(void)
{
    assert(strcmp(ina3221_power_ux_state_name(INA3221_POWER_UX_BATTERY_CRITICAL), "BATTERY_CRITICAL") == 0);
    assert(strcmp(ina3221_power_ux_state_name(INA3221_POWER_UX_USB_ONLY_NO_BATTERY), "USB_ONLY_NO_BATTERY") == 0);
    assert(strcmp(ina3221_power_ux_state_name((ina3221_power_ux_state_t)99), "UNKNOWN") == 0);
    assert(ina3221_power_ux_update(NULL, &cfg, NULL) == INA3221_POWER_UX_UNKNOWN);
}

static void test_evidence_changes(void)
{
    ina3221_power_evidence_t a = { .observed = true, .battery_present = true, .usb_present = false, .charging = false, .soc_quality = 1 };
    ina3221_power_evidence_t b = a;
    assert(ina3221_power_evidence_changes(&a, &b) == 0U);
    b.usb_present = true;
    b.charging = true;
    assert(ina3221_power_evidence_changes(&a, &b) == (INA3221_EVIDENCE_USB_CHANGED | INA3221_EVIDENCE_CHARGING_CHANGED));
    b = a;
    b.battery_present = false;
    b.soc_quality = 0;
    assert(ina3221_power_evidence_changes(&a, &b) == (INA3221_EVIDENCE_BATTERY_CHANGED | INA3221_EVIDENCE_SOC_CHANGED));
    /* First observation: nothing to compare with. */
    ina3221_power_evidence_t none = { 0 };
    assert(ina3221_power_evidence_changes(&none, &a) == 0U);
    assert(ina3221_power_evidence_changes(NULL, &a) == 0U);
}

int main(void)
{
    test_config();
    test_usb_states();
    test_battery_bands_with_hysteresis();
    test_time_qualification();
    test_names();
    test_evidence_changes();
    return 0;
}
