#include <assert.h>
#include <math.h>
#include <string.h>

#include "ina3221_soc.h"

/* MAX-181: hybrid voltage + coulomb-counting state of charge. */

static ina3221_soc_config_t cfg;

static bool near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

static ina3221_soc_input_t sample(float v, float i)
{
    return (ina3221_soc_input_t) { .valid = true, .voltage_v = v, .current_a = i };
}

static void setup(float capacity)
{
    ina3221_soc_default_config(&cfg);
    cfg.capacity_mah = capacity;
}

static void test_curve(void)
{
    static const struct { float voltage_v; float percent; } knots[] = {
        { 3.00f, 0.0f }, { 3.30f, 2.0f }, { 3.50f, 7.0f },
        { 3.60f, 15.0f }, { 3.70f, 35.0f }, { 3.80f, 55.0f },
        { 3.90f, 70.0f }, { 4.00f, 80.0f }, { 4.10f, 90.0f },
        { 4.20f, 100.0f },
    };
    assert(ina3221_soc_voltage_percent(2.9f) == 0.0f);
    assert(ina3221_soc_voltage_percent(4.3f) == 100.0f);
    assert(near(ina3221_soc_voltage_percent(3.75f), 45.0f, 0.01f));
    assert(isnan(ina3221_soc_voltage_percent(NAN)));
    for (size_t i = 0U; i < sizeof(knots) / sizeof(knots[0]); ++i) {
        assert(near(ina3221_soc_voltage_percent(knots[i].voltage_v), knots[i].percent, 0.001f));
        if (i == 0U) {
            assert(ina3221_soc_voltage_percent(knots[i].voltage_v - 0.001f) == 0.0f);
            const float slope = (knots[i + 1U].percent - knots[i].percent) /
                (knots[i + 1U].voltage_v - knots[i].voltage_v);
            assert(near(ina3221_soc_voltage_percent(knots[i].voltage_v + 0.001f),
                        knots[i].percent + slope * 0.001f, 0.001f));
        } else if (i + 1U == sizeof(knots) / sizeof(knots[0])) {
            const float slope = (knots[i].percent - knots[i - 1U].percent) /
                (knots[i].voltage_v - knots[i - 1U].voltage_v);
            assert(near(ina3221_soc_voltage_percent(knots[i].voltage_v - 0.001f),
                        knots[i].percent - slope * 0.001f, 0.001f));
            assert(ina3221_soc_voltage_percent(knots[i].voltage_v + 0.001f) == 100.0f);
        } else {
            const float lower_slope = (knots[i].percent - knots[i - 1U].percent) /
                (knots[i].voltage_v - knots[i - 1U].voltage_v);
            const float upper_slope = (knots[i + 1U].percent - knots[i].percent) /
                (knots[i + 1U].voltage_v - knots[i].voltage_v);
            assert(near(ina3221_soc_voltage_percent(knots[i].voltage_v - 0.001f),
                        knots[i].percent - lower_slope * 0.001f, 0.001f));
            assert(near(ina3221_soc_voltage_percent(knots[i].voltage_v + 0.001f),
                        knots[i].percent + upper_slope * 0.001f, 0.001f));
        }
    }
}

static void test_unknown_capacity_stays_voltage_only(void)
{
    setup(0.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    for (int64_t t = 0; t < 100000; t += 1000) {
        ina3221_soc_input_t in = sample(3.80f, 0.01f);
        ina3221_soc_update(&soc, &cfg, &in, t);
    }
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    assert(near(ina3221_soc_percent(&soc, &cfg), 55.0f, 0.01f));
}

static void test_bootstrap_only_at_rest(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t loaded = sample(3.60f, 0.8f); /* sagging under load */
    ina3221_soc_update(&soc, &cfg, &loaded, 0);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    ina3221_soc_input_t rest = sample(3.80f, 0.02f);
    ina3221_soc_update(&soc, &cfg, &rest, 1000);
    assert(soc.quality == INA3221_SOC_COULOMB_TRACKED);
    assert(near(ina3221_soc_percent(&soc, &cfg), 55.0f, 0.01f));
    assert(near(soc.charge_mah, 1650.0f, 0.1f));
}

static void test_discharge_and_charge_integration_with_irregular_intervals(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);               /* 1650 mAh */

    /* 1 A discharge for 3.6 s total, in uneven steps: 1 mAh. */
    const int64_t steps[] = { 1000, 500, 2100 };
    int64_t t = 0;
    for (size_t i = 0; i < 3U; ++i) {
        t += steps[i];
        ina3221_soc_input_t d = sample(3.70f, 1.0f);
        ina3221_soc_update(&soc, &cfg, &d, t);
    }
    assert(near(soc.charge_mah, 1649.0f, 0.01f));
    assert(soc.quality == INA3221_SOC_COULOMB_TRACKED);

    /* Charging (negative current) adds charge: 2 A for 3.6 s = +2 mAh. */
    for (int k = 0; k < 4; ++k) {
        t += 900;
        ina3221_soc_input_t c = sample(3.95f, -2.0f);
        c.usb_present = true;
        c.charging = true;
        ina3221_soc_update(&soc, &cfg, &c, t);
    }
    assert(near(soc.charge_mah, 1651.0f, 0.01f));
}

static void test_gap_is_not_integrated_as_zero_current(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);
    ina3221_soc_input_t d = sample(3.70f, 1.0f);
    ina3221_soc_update(&soc, &cfg, &d, 1000);
    /* INA3221 missing for 60 s while a load was running. */
    ina3221_soc_input_t invalid = { .valid = false };
    ina3221_soc_update(&soc, &cfg, &invalid, 2000);
    ina3221_soc_update(&soc, &cfg, &d, 62000);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    assert(near(ina3221_soc_percent(&soc, &cfg), ina3221_soc_voltage_percent(3.70f), 0.01f));
    /* Next rest re-anchors tracking. */
    ina3221_soc_update(&soc, &cfg, &rest, 63000);
    assert(soc.quality == INA3221_SOC_COULOMB_TRACKED);

    /* A clock step backwards is a gap too. */
    ina3221_soc_update(&soc, &cfg, &d, 10000);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
}

/* A sample may be up to 10 s late (the rail stale window) without losing coulomb tracking. */
static void test_default_gap_window_is_ten_seconds(void)
{
    setup(3000.0f);
    assert(cfg.max_gap_ms == 10000U);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);
    ina3221_soc_input_t load = sample(3.70f, 1.0f);
    ina3221_soc_update(&soc, &cfg, &load, 1000);
    ina3221_soc_update(&soc, &cfg, &load, 11000);
    assert(soc.quality == INA3221_SOC_COULOMB_TRACKED);
    ina3221_soc_update(&soc, &cfg, &load, 21001);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
}

static void test_zero_time_huge_gap_and_reboot_do_not_integrate_stale_current(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    const ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);

    const ina3221_soc_input_t discharge = sample(3.70f, 1.0f);
    ina3221_soc_update(&soc, &cfg, &discharge, 1000);
    const float after_one_second = soc.charge_mah;
    ina3221_soc_input_t duplicate = sample(3.70f, 50.0f);
    ina3221_soc_update(&soc, &cfg, &duplicate, 1000);
    assert(near(soc.charge_mah, after_one_second, 0.001f));

    const ina3221_soc_input_t long_gap = sample(3.70f, 50.0f);
    ina3221_soc_update(&soc, &cfg, &long_gap, 100000);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    assert(near(soc.charge_mah, after_one_second, 0.001f));

    /* The runtime estimator is reconstructed after reboot; no off-time current is inferred. */
    ina3221_soc_reset(&soc);
    ina3221_soc_update(&soc, &cfg, &discharge, 0);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    assert(soc.charge_mah == 0.0f);
    assert(near(ina3221_soc_percent(&soc, &cfg), ina3221_soc_voltage_percent(3.70f), 0.001f));
}

static void test_invalid_sample_breaks_tracking_and_hides_percent(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    const ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);
    const ina3221_soc_input_t discharge = sample(3.70f, 1.0f);
    ina3221_soc_update(&soc, &cfg, &discharge, 1000);
    const float charge_before_gap = soc.charge_mah;

    const ina3221_soc_input_t invalid = { .valid = false };
    ina3221_soc_update(&soc, &cfg, &invalid, 2000);
    assert(soc.quality == INA3221_SOC_INVALID);
    assert(isnan(ina3221_soc_percent(&soc, &cfg)));

    /* Even a short hole cannot make the next sample integrate across it. */
    const ina3221_soc_input_t recovered = sample(3.70f, 50.0f);
    ina3221_soc_update(&soc, &cfg, &recovered, 3000);
    assert(soc.quality == INA3221_SOC_VOLTAGE_ONLY);
    assert(near(soc.charge_mah, charge_before_gap, 0.001f));
    assert(near(ina3221_soc_percent(&soc, &cfg), ina3221_soc_voltage_percent(3.70f), 0.001f));
}

static void test_full_anchor_and_clamps(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t full = sample(4.15f, 0.01f);
    full.usb_present = true;
    full.charging = false;
    ina3221_soc_update(&soc, &cfg, &full, 0);
    assert(soc.quality == INA3221_SOC_FULL_ANCHORED);
    assert(near(ina3221_soc_percent(&soc, &cfg), 100.0f, 0.001f));

    /* Charging current at full never exceeds capacity. */
    ina3221_soc_input_t over = sample(4.18f, -1.0f);
    over.usb_present = true;
    over.charging = true;
    ina3221_soc_update(&soc, &cfg, &over, 1000);
    assert(soc.charge_mah <= cfg.capacity_mah);

    /* Deep discharge clamps at 0. */
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t low = sample(3.30f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &low, 0);
    /* 2 % of 3000 mAh = 60 mAh; 1.6 A for 300 s = 133 mAh. */
    for (int64_t t = 1000; t <= 300000; t += 1000) {
        ina3221_soc_input_t d = sample(3.20f, 1.6f);
        ina3221_soc_update(&soc, &cfg, &d, t);
    }
    assert(soc.charge_mah == 0.0f);
    assert(ina3221_soc_percent(&soc, &cfg) == 0.0f);
}

static void test_long_rest_reanchors_drift(void)
{
    setup(3000.0f);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    ina3221_soc_input_t rest = sample(3.80f, 0.0f);
    ina3221_soc_update(&soc, &cfg, &rest, 0);
    soc.charge_mah = 2500.0f; /* integration drift */
    int64_t t = 0;
    for (; t < (int64_t)cfg.rest_anchor_ms - 1000; t += 1000) {
        ina3221_soc_update(&soc, &cfg, &rest, t + 1000);
    }
    assert(soc.charge_mah > 2400.0f); /* not yet */
    ina3221_soc_update(&soc, &cfg, &rest, t + 1000);
    ina3221_soc_update(&soc, &cfg, &rest, t + 2000);
    assert(near(soc.charge_mah, 1650.0f, 1.0f));
}

static void test_names_and_guards(void)
{
    assert(strcmp(ina3221_soc_quality_name(INA3221_SOC_FULL_ANCHORED), "FULL_ANCHORED") == 0);
    assert(strcmp(ina3221_soc_quality_name(INA3221_SOC_INVALID), "INVALID") == 0);
    ina3221_soc_t soc;
    ina3221_soc_reset(&soc);
    assert(isnan(ina3221_soc_percent(&soc, &cfg)));
    ina3221_soc_update(NULL, &cfg, NULL, 0);
}

int main(void)
{
    test_curve();
    test_unknown_capacity_stays_voltage_only();
    test_bootstrap_only_at_rest();
    test_discharge_and_charge_integration_with_irregular_intervals();
    test_gap_is_not_integrated_as_zero_current();
    test_default_gap_window_is_ten_seconds();
    test_zero_time_huge_gap_and_reboot_do_not_integrate_stale_current();
    test_invalid_sample_breaks_tracking_and_hides_percent();
    test_full_anchor_and_clamps();
    test_long_rest_reanchors_drift();
    test_names_and_guards();
    return 0;
}
