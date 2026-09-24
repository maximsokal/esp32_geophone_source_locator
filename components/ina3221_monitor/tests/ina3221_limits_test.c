#include <assert.h>
#include <math.h>
#include <string.h>

#include "ina3221_limits.h"

/* MAX-245: INA3221 over-current limits and software reaction. */

#define SHUNT 0.1f

static ina3221_channel_sample_t samples[INA3221_POLICY_CHANNEL_COUNT];

static void set_currents(float battery, float esp32, float ad7606)
{
    memset(samples, 0, sizeof(samples));
    samples[0] = (ina3221_channel_sample_t) { .valid = true, .bus_voltage_v = 3.9f, .current_a = battery };
    samples[1] = (ina3221_channel_sample_t) { .valid = true, .bus_voltage_v = 5.0f, .current_a = esp32 };
    samples[2] = (ina3221_channel_sample_t) { .valid = true, .bus_voltage_v = 5.0f, .current_a = ad7606 };
}

static void test_register_encoding(void)
{
    /* 1.5 A * 0.1 Ohm = 150 mV = 3750 counts of 40 uV, in bits 15..3. */
    assert(ina3221_limits_register_value(1.5f, SHUNT) == (uint16_t)(3750 * 8));
    assert(ina3221_limits_register_value(1.2f, SHUNT) == (uint16_t)(3000 * 8));
    assert(ina3221_limits_register_value(0.0f, SHUNT) == 0x0000U);
    /* Negative limits are two's complement. */
    assert(ina3221_limits_register_value(-0.1f, SHUNT) == (uint16_t)(int16_t)(-250 * 8));
    /* Clamped to the 13-bit range. */
    assert(ina3221_limits_register_value(100.0f, SHUNT) == 0x7FF8U);
    assert(ina3221_limits_register_value(-100.0f, SHUNT) == 0x8000U);
    assert(ina3221_limits_register_value(NAN, SHUNT) == INA3221_LIMITS_REG_DISABLED);
    assert(ina3221_limits_register_value(1.0f, 0.0f) == INA3221_LIMITS_REG_DISABLED);
}

static void test_defaults_and_hardware_programming(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    assert(ina3221_limits_valid(&limits, SHUNT));

    uint16_t crit;
    uint16_t warn;
    /* Battery shunt reads discharge positive: hardware limits programmed. */
    ina3221_limits_channel_registers(&limits, 0U, SHUNT, &crit, &warn);
    assert(crit == ina3221_limits_register_value(1.50f, SHUNT));
    assert(warn == ina3221_limits_register_value(1.20f, SHUNT));
    /* Rail shunts read load current negative: a positive comparator limit
     * could never trip, so the hardware comparators are disabled. */
    for (size_t c = 1U; c < INA3221_POLICY_CHANNEL_COUNT; ++c) {
        ina3221_limits_channel_registers(&limits, c, SHUNT, &crit, &warn);
        assert(crit == INA3221_LIMITS_REG_DISABLED && warn == INA3221_LIMITS_REG_DISABLED);
    }
    ina3221_limits_channel_registers(&limits, 7U, SHUNT, &crit, &warn);
    assert(crit == INA3221_LIMITS_REG_DISABLED);
}

static void test_invalid_limits_are_rejected(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    limits.channels[2].critical_a = limits.channels[2].warning_a; /* not above warning */
    assert(!ina3221_limits_valid(&limits, SHUNT));
    ina3221_limits_default(&limits);
    limits.channels[0].critical_a = 2.0f; /* beyond 1.638 A full scale */
    assert(!ina3221_limits_valid(&limits, SHUNT));
    ina3221_limits_default(&limits);
    limits.channels[1].warning_a = 0.0f;
    assert(!ina3221_limits_valid(&limits, SHUNT));
    assert(!ina3221_limits_valid(NULL, SHUNT));
    ina3221_limits_default(&limits);
    assert(!ina3221_limits_valid(&limits, 0.0f));
}

static void test_critical_needs_confirmation_and_latches(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    ina3221_overcurrent_tracker_t t;
    ina3221_overcurrent_reset(&t);

    set_currents(0.3f, 0.3f, 0.30f); /* AD7606 rail short: 300 mA */
    assert(!ina3221_overcurrent_update(&t, &limits, samples)); /* one sample: not yet */
    assert(t.channels[2].level != INA3221_OVERCURRENT_CRITICAL);
    assert(ina3221_overcurrent_update(&t, &limits, samples));  /* confirmed */
    assert(t.channels[2].level == INA3221_OVERCURRENT_CRITICAL);
    assert(t.channels[2].latched);
    assert(fabsf(t.channels[2].latched_current_a - 0.30f) < 1e-6f);
    assert(!ina3221_overcurrent_update(&t, &limits, samples));  /* reported once */

    /* The fault going away does not unlatch. */
    for (int i = 0; i < 10; ++i) {
        set_currents(0.3f, 0.3f, 0.02f);
        (void)ina3221_overcurrent_update(&t, &limits, samples);
    }
    assert(t.channels[2].level == INA3221_OVERCURRENT_CRITICAL && t.channels[2].latched);
    /* Other channels unaffected. */
    assert(t.channels[0].level == INA3221_OVERCURRENT_NONE);
    assert(t.channels[1].level == INA3221_OVERCURRENT_NONE);

    ina3221_overcurrent_clear_latch(&t, 2U);
    assert(t.channels[2].level == INA3221_OVERCURRENT_NONE && !t.channels[2].latched);
    ina3221_overcurrent_clear_latch(&t, 9U); /* ignored */
}

static void test_single_spike_and_invalid_samples(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    ina3221_overcurrent_tracker_t t;
    ina3221_overcurrent_reset(&t);

    set_currents(0.3f, 2.0f, 0.02f); /* one Wi-Fi-like spike on ESP32 5 V */
    assert(!ina3221_overcurrent_update(&t, &limits, samples));
    set_currents(0.3f, 0.3f, 0.02f);
    assert(!ina3221_overcurrent_update(&t, &limits, samples));
    set_currents(0.3f, 2.0f, 0.02f);
    assert(!ina3221_overcurrent_update(&t, &limits, samples));
    assert(t.channels[1].level == INA3221_OVERCURRENT_NONE);

    /* An invalid sample between two high ones neither confirms nor resets. */
    set_currents(0.3f, 2.0f, 0.02f);
    samples[1].valid = false;
    assert(!ina3221_overcurrent_update(&t, &limits, samples));
    set_currents(0.3f, 2.0f, 0.02f);
    assert(ina3221_overcurrent_update(&t, &limits, samples));
    assert(t.channels[1].latched);
}

static void test_reverse_current_counts_by_magnitude(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    ina3221_overcurrent_tracker_t t;
    ina3221_overcurrent_reset(&t);
    set_currents(-1.6f, 0.3f, 0.02f); /* battery charging/back-feed at 1.6 A */
    (void)ina3221_overcurrent_update(&t, &limits, samples);
    assert(ina3221_overcurrent_update(&t, &limits, samples));
    assert(t.channels[0].level == INA3221_OVERCURRENT_CRITICAL);
}

static void test_warning_hysteresis(void)
{
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    ina3221_overcurrent_tracker_t t;
    ina3221_overcurrent_reset(&t);

    /* ESP32 rail: warning 0.80 A, clear below 0.72 A. */
    for (int i = 0; i < 2; ++i) {
        set_currents(0.3f, 0.85f, 0.02f);
        assert(!ina3221_overcurrent_update(&t, &limits, samples));
    }
    assert(t.channels[1].level == INA3221_OVERCURRENT_WARNING);
    assert(!t.channels[1].latched);

    /* Between 0.72 and 0.80 A the warning holds. */
    for (int i = 0; i < 5; ++i) {
        set_currents(0.3f, 0.75f, 0.02f);
        (void)ina3221_overcurrent_update(&t, &limits, samples);
    }
    assert(t.channels[1].level == INA3221_OVERCURRENT_WARNING);

    /* One low sample is not enough to clear; two are. */
    set_currents(0.3f, 0.5f, 0.02f);
    (void)ina3221_overcurrent_update(&t, &limits, samples);
    assert(t.channels[1].level == INA3221_OVERCURRENT_WARNING);
    (void)ina3221_overcurrent_update(&t, &limits, samples);
    assert(t.channels[1].level == INA3221_OVERCURRENT_NONE);
}

static void test_names_and_guards(void)
{
    assert(strcmp(ina3221_overcurrent_level_name(INA3221_OVERCURRENT_NONE), "none") == 0);
    assert(strcmp(ina3221_overcurrent_level_name(INA3221_OVERCURRENT_WARNING), "warning") == 0);
    assert(strcmp(ina3221_overcurrent_level_name(INA3221_OVERCURRENT_CRITICAL), "critical") == 0);
    ina3221_limits_t limits;
    ina3221_limits_default(&limits);
    assert(!ina3221_overcurrent_update(NULL, &limits, samples));
    ina3221_overcurrent_reset(NULL);
    ina3221_limits_default(NULL);
}

int main(void)
{
    test_register_encoding();
    test_defaults_and_hardware_programming();
    test_invalid_limits_are_rejected();
    test_critical_needs_confirmation_and_latches();
    test_single_spike_and_invalid_samples();
    test_reverse_current_counts_by_magnitude();
    test_warning_hysteresis();
    test_names_and_guards();
    return 0;
}
