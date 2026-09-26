#include <assert.h>
#include <math.h>
#include <string.h>

#include "ina3221_power_ux.h"

/* MAX-180: user-facing power states, hysteresis and time qualification. */

static ina3221_power_ux_config_t cfg;
static uint64_t fake_now_ms;

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
        s = ina3221_power_ux_update(ux, &cfg, &in, fake_now_ms);
        fake_now_ms += 1000U;
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
    bad = cfg;
    bad.critical_confirm_dwell_ms = bad.confirm_dwell_ms - 1U;
    assert(!ina3221_power_ux_config_valid(&bad));
    bad = cfg;
    bad.full_exit_v = bad.full_v;
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
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.45f}) == INA3221_POWER_UX_BATTERY);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.44f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.30f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.29f}) == INA3221_POWER_UX_BATTERY_CRITICAL);
    /* Leaving LOW needs 3.60 V, leaving CRITICAL needs 3.45 V. */
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.55f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.60f}) == INA3221_POWER_UX_BATTERY);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.59f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_LOW, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.61f}) == INA3221_POWER_UX_BATTERY);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.40f}) == INA3221_POWER_UX_BATTERY_CRITICAL);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.45f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.44f}) == INA3221_POWER_UX_BATTERY_CRITICAL);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.50f}) == INA3221_POWER_UX_BATTERY_LOW);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY_CRITICAL, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.70f}) == INA3221_POWER_UX_BATTERY);
    /* Battery source without a plausible pack voltage is not a state. */
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, false, 0.0f}) == INA3221_POWER_UX_UNKNOWN);
    assert(ina3221_power_ux_classify(&cfg, INA3221_POWER_UX_BATTERY, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, NAN}) == INA3221_POWER_UX_UNKNOWN);
}

static void test_time_qualification(void)
{
    fake_now_ms = 0U;
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);

    /* Booting on a nearly empty pack: never CRITICAL from one sample. */
    assert(ina3221_power_ux_update(&ux, &cfg, &(ina3221_power_ux_input_t){INA3221_POWER_STATE_BATTERY, true, true, 3.20f}, fake_now_ms) == INA3221_POWER_UX_BATTERY);
    fake_now_ms += 1000U;
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

static void test_dwell_is_time_bounded_during_fast_sampling(void)
{
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);
    uint64_t now_ms = 0U;
    const ina3221_power_ux_input_t normal = on_battery(3.80f);
    assert(ina3221_power_ux_update(&ux, &cfg, &normal, now_ms) == INA3221_POWER_UX_BATTERY);

    const ina3221_power_ux_input_t low = on_battery(3.40f);
    for (unsigned i = 1U; i <= 100U; ++i) {
        now_ms = (uint64_t)i * 20U;
        assert(ina3221_power_ux_update(&ux, &cfg, &low, now_ms) == INA3221_POWER_UX_BATTERY);
    }
    now_ms += 20U;
    assert(ina3221_power_ux_update(&ux, &cfg, &low, now_ms) == INA3221_POWER_UX_BATTERY_LOW);

    const ina3221_power_ux_input_t critical = on_battery(3.20f);
    for (unsigned i = 1U; i <= 200U; ++i) {
        now_ms += 20U;
        const ina3221_power_ux_state_t state = ina3221_power_ux_update(&ux, &cfg, &critical, now_ms);
        assert(state == INA3221_POWER_UX_BATTERY_LOW);
    }
    now_ms += 20U;
    assert(ina3221_power_ux_update(&ux, &cfg, &critical, now_ms) == INA3221_POWER_UX_BATTERY_CRITICAL);
}

static void test_full_state_hysteresis_and_dwell(void)
{
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);
    ina3221_power_ux_input_t idle = {
        .source = INA3221_POWER_STATE_USB,
        .telemetry_valid = true,
        .battery_present = true,
        .battery_voltage_v = 4.099f,
    };

    for (uint64_t now_ms = 0U; now_ms <= 2000U; now_ms += 20U) {
        assert(ina3221_power_ux_update(&ux, &cfg, &idle, now_ms) == INA3221_POWER_UX_USB_BATTERY_READY);
        assert(!ux.battery_full);
    }

    idle.battery_voltage_v = cfg.full_v;
    for (uint64_t now_ms = 2020U; now_ms < 4020U; now_ms += 20U) {
        (void)ina3221_power_ux_update(&ux, &cfg, &idle, now_ms);
        assert(!ux.battery_full);
    }
    (void)ina3221_power_ux_update(&ux, &cfg, &idle, 4020U);
    assert(ux.battery_full);

    idle.battery_voltage_v = cfg.full_exit_v + 0.001f;
    (void)ina3221_power_ux_update(&ux, &cfg, &idle, 4040U);
    assert(ux.battery_full);
    idle.battery_voltage_v = cfg.full_exit_v - 0.001f;
    (void)ina3221_power_ux_update(&ux, &cfg, &idle, 4060U); /* one-sample dip */
    idle.battery_voltage_v = cfg.full_exit_v + 0.001f;
    (void)ina3221_power_ux_update(&ux, &cfg, &idle, 4080U);
    assert(ux.battery_full);

    idle.battery_voltage_v = cfg.full_exit_v;
    for (uint64_t now_ms = 4100U; now_ms < 6100U; now_ms += 20U) {
        (void)ina3221_power_ux_update(&ux, &cfg, &idle, now_ms);
        assert(ux.battery_full);
    }
    (void)ina3221_power_ux_update(&ux, &cfg, &idle, 6100U);
    assert(!ux.battery_full);

    idle.telemetry_valid = false;
    ux.battery_full = true;
    assert(ina3221_power_ux_update(&ux, &cfg, &idle, 6120U) == INA3221_POWER_UX_UNKNOWN);
    assert(!ux.battery_full);
}

static uint32_t noise_next(uint32_t *seed)
{
    *seed = *seed * 1664525U + 1013904223U;
    return *seed;
}

static void test_invalid_telemetry_is_immediately_unknown(void)
{
    fake_now_ms = 0U;
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);
    assert(feed(&ux, on_battery(3.90f), 3) == INA3221_POWER_UX_BATTERY);

    ina3221_power_ux_input_t invalid = on_battery(3.90f);
    invalid.telemetry_valid = false;
    assert(ina3221_power_ux_update(&ux, &cfg, &invalid, fake_now_ms) == INA3221_POWER_UX_UNKNOWN);

    ina3221_power_ux_input_t source_unknown = on_battery(3.90f);
    source_unknown.source = INA3221_POWER_STATE_UNKNOWN;
    assert(ina3221_power_ux_update(&ux, &cfg, &source_unknown, fake_now_ms) == INA3221_POWER_UX_UNKNOWN);

    ina3221_power_ux_reset(&ux);
    fake_now_ms = 0U;
    assert(feed(&ux, on_battery(3.80f), 3) == INA3221_POWER_UX_BATTERY);
    assert(feed(&ux, on_battery(3.20f), 4) == INA3221_POWER_UX_BATTERY);
    invalid = on_battery(3.80f);
    invalid.telemetry_valid = false;
    assert(ina3221_power_ux_update(&ux, &cfg, &invalid, fake_now_ms) == INA3221_POWER_UX_UNKNOWN);
    assert(feed(&ux, on_battery(3.20f), 4) == INA3221_POWER_UX_UNKNOWN);
    assert(feed(&ux, on_battery(3.20f), 1) == INA3221_POWER_UX_BATTERY_CRITICAL);
}

static void test_seeded_noise_is_filtered_for_voltage_and_current(void)
{
    fake_now_ms = 0U;
    ina3221_power_ux_t ux;
    ina3221_power_ux_reset(&ux);
    assert(feed(&ux, on_battery(3.80f), 3) == INA3221_POWER_UX_BATTERY);

    uint32_t seed = 0x179275U;
    unsigned raw_voltage_transitions = 0U;
    unsigned filtered_transitions = 0U;
    ina3221_power_ux_state_t previous_raw = INA3221_POWER_UX_BATTERY;
    for (unsigned i = 0U; i < 500U; ++i) {
        const float noise = ((float)(noise_next(&seed) >> 16) / 32767.5f - 1.0f) * 0.02f;
        const ina3221_power_ux_input_t input = on_battery(3.45f + noise);
        const ina3221_power_ux_state_t raw = ina3221_power_ux_classify(
            &cfg,
            INA3221_POWER_UX_BATTERY,
            &input
        );
        if (raw != previous_raw) raw_voltage_transitions++;
        previous_raw = raw;
        const ina3221_power_ux_state_t before = ux.state;
        (void)ina3221_power_ux_update(&ux, &cfg, &input, fake_now_ms);
        if (ux.state != before) filtered_transitions++;
        fake_now_ms += 20U;
    }
    assert(raw_voltage_transitions > 40U);
    assert(filtered_transitions == 0U);

    ina3221_power_classifier_t source_filter = {0};
    source_filter.initialized = true;
    source_filter.stable_state = INA3221_POWER_STATE_BATTERY;
    source_filter.candidate_state = INA3221_POWER_STATE_BATTERY;
    ux.state = INA3221_POWER_UX_BATTERY;
    ux.candidate = INA3221_POWER_UX_BATTERY;
    ux.candidate_count = 0U;
    ux.candidate_since_valid = false;
    unsigned raw_current_transitions = 0U;
    unsigned filtered_current_transitions = 0U;
    ina3221_power_state_t previous_source = INA3221_POWER_STATE_BATTERY;
    for (unsigned i = 0U; i < 2000U; ++i) {
        ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT] = {
            { .valid = true, .bus_voltage_v = 3.8f },
            { .valid = true, .bus_voltage_v = 5.0f },
            { .valid = true, .bus_voltage_v = 5.0f },
        };
        const float noise = (float)(noise_next(&seed) >> 16) / 32767.5f - 1.0f;
        channels[INA3221_POLICY_BATTERY_INDEX].current_a = noise * 0.06f;
        const ina3221_power_state_t raw_source = ina3221_policy_classify_instant(channels);
        if (raw_source != previous_source) raw_current_transitions++;
        previous_source = raw_source;

        const ina3221_power_state_t stable_source = ina3221_policy_classifier_update(
            &source_filter,
            raw_source
        );
        const ina3221_power_ux_state_t before = ux.state;
        const ina3221_power_ux_input_t input = {
            .source = stable_source,
            .telemetry_valid = true,
            .battery_present = true,
            .battery_voltage_v = 3.80f,
        };
        (void)ina3221_power_ux_update(&ux, &cfg, &input, fake_now_ms);
        if (ux.state != before) filtered_current_transitions++;
        fake_now_ms += 20U;
    }
    assert(raw_current_transitions > 100U);
    assert(filtered_current_transitions < raw_current_transitions / 20U);
}

static void test_names(void)
{
    static const struct {
        ina3221_power_ux_state_t state;
        const char *name;
    } names[] = {
        { INA3221_POWER_UX_USB_ONLY_NO_BATTERY, "USB_ONLY_NO_BATTERY" },
        { INA3221_POWER_UX_USB_CHARGING, "USB_CHARGING" },
        { INA3221_POWER_UX_USB_BATTERY_READY, "USB_BATTERY_READY" },
        { INA3221_POWER_UX_BATTERY, "BATTERY" },
        { INA3221_POWER_UX_BATTERY_LOW, "BATTERY_LOW" },
        { INA3221_POWER_UX_BATTERY_CRITICAL, "BATTERY_CRITICAL" },
        { INA3221_POWER_UX_UNKNOWN, "UNKNOWN" },
    };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(strcmp(ina3221_power_ux_state_name(names[i].state), names[i].name) == 0);
    }
    assert(strcmp(ina3221_power_ux_state_name((ina3221_power_ux_state_t)99), "UNKNOWN") == 0);
    assert(ina3221_power_ux_update(NULL, &cfg, NULL, 0U) == INA3221_POWER_UX_UNKNOWN);
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
    test_dwell_is_time_bounded_during_fast_sampling();
    test_full_state_hysteresis_and_dwell();
    test_invalid_telemetry_is_immediately_unknown();
    test_seeded_noise_is_filtered_for_voltage_and_current();
    test_names();
    test_evidence_changes();
    return 0;
}
