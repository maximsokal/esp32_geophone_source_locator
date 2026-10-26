#include "ina3221_limits.h"

#include <math.h>
#include <string.h>

#define INA3221_LIMITS_SHUNT_LSB_V 0.000040f
#define INA3221_LIMITS_SHUNT_FULL_SCALE_V 0.16376f
#define INA3221_LIMITS_MAX_COUNTS 4095


void ina3221_limits_default(ina3221_limits_t *limits)
{
    if (limits == NULL) {
        return;
    }
    memset(limits, 0, sizeof(*limits));
    /* Battery (1S Li-ion into the 5 V boost): discharge current. */
    limits->channels[INA3221_POLICY_BATTERY_INDEX] =
        (ina3221_channel_limit_t) { .warning_a = 1.20f, .critical_a = 1.50f, .program_hardware = true };
    /* ESP32-S3 + TFT + SD + Wi-Fi bursts. */
    limits->channels[INA3221_POLICY_ESP32_INDEX] =
        (ina3221_channel_limit_t) { .warning_a = 0.80f, .critical_a = 1.20f, .program_hardware = false };
    /* AD7606 module + LD1117V33 VIO regulator. */
    limits->channels[INA3221_POLICY_AD7606_INDEX] =
        (ina3221_channel_limit_t) { .warning_a = 0.15f, .critical_a = 0.25f, .program_hardware = false };
}


bool ina3221_limits_valid(const ina3221_limits_t *limits, float shunt_ohm)
{
    if (limits == NULL || !(shunt_ohm > 0.0f)) {
        return false;
    }
    const float full_scale_a = INA3221_LIMITS_SHUNT_FULL_SCALE_V / shunt_ohm;
    for (size_t i = 0; i < INA3221_POLICY_CHANNEL_COUNT; ++i) {
        const ina3221_channel_limit_t *c = &limits->channels[i];
        if (!(c->warning_a > 0.0f) || !(c->critical_a > c->warning_a) ||
            !(c->critical_a <= full_scale_a) || !isfinite(c->critical_a)) {
            return false;
        }
    }
    return true;
}


uint16_t ina3221_limits_register_value(float current_a, float shunt_ohm)
{
    if (!(shunt_ohm > 0.0f) || !isfinite(shunt_ohm) || !isfinite(current_a)) {
        return INA3221_LIMITS_REG_DISABLED;
    }
    float counts_f = (current_a * shunt_ohm) / INA3221_LIMITS_SHUNT_LSB_V;
    /* Saturate before the float-to-int conversion, including product overflow. */
    if (counts_f >= INA3221_LIMITS_MAX_COUNTS) return 0x7FF8U;
    if (counts_f <= -INA3221_LIMITS_MAX_COUNTS - 1) return 0x8000U;
    counts_f = counts_f >= 0.0f ? floorf(counts_f + 0.5f) : ceilf(counts_f - 0.5f);
    int counts = (int)counts_f;
    if (counts > INA3221_LIMITS_MAX_COUNTS) {
        counts = INA3221_LIMITS_MAX_COUNTS;
    }
    if (counts < -INA3221_LIMITS_MAX_COUNTS - 1) {
        counts = -INA3221_LIMITS_MAX_COUNTS - 1;
    }
    return (uint16_t)((int16_t)(counts * 8));
}


void ina3221_limits_channel_registers(
    const ina3221_limits_t *limits,
    size_t channel,
    float shunt_ohm,
    uint16_t *critical_reg,
    uint16_t *warning_reg)
{
    *critical_reg = INA3221_LIMITS_REG_DISABLED;
    *warning_reg = INA3221_LIMITS_REG_DISABLED;
    if (limits == NULL || channel >= INA3221_POLICY_CHANNEL_COUNT) {
        return;
    }
    const ina3221_channel_limit_t *c = &limits->channels[channel];
    if (!c->program_hardware) {
        return;
    }
    *critical_reg = ina3221_limits_register_value(c->critical_a, shunt_ohm);
    *warning_reg = ina3221_limits_register_value(c->warning_a, shunt_ohm);
}


void ina3221_overcurrent_reset(ina3221_overcurrent_tracker_t *tracker)
{
    if (tracker != NULL) {
        memset(tracker, 0, sizeof(*tracker));
    }
}


static uint8_t streak_inc(uint8_t value)
{
    return value < 0xFFU ? (uint8_t)(value + 1U) : value;
}


bool ina3221_overcurrent_update(
    ina3221_overcurrent_tracker_t *tracker,
    const ina3221_limits_t *limits,
    const ina3221_channel_sample_t samples[INA3221_POLICY_CHANNEL_COUNT])
{
    bool newly_latched = false;
    if (tracker == NULL || limits == NULL || samples == NULL) {
        return false;
    }
    for (size_t i = 0; i < INA3221_POLICY_CHANNEL_COUNT; ++i) {
        ina3221_overcurrent_channel_t *ch = &tracker->channels[i];
        const ina3221_channel_limit_t *lim = &limits->channels[i];
        const ina3221_channel_sample_t *s = &samples[i];
        if (!s->valid || !isfinite(s->current_a)) {
            continue; /* no evidence either way */
        }
        /* Magnitude: a reverse short is as dangerous as a forward one. */
        const float current = fabsf(s->current_a);

        ch->critical_streak = current >= lim->critical_a ? streak_inc(ch->critical_streak) : 0U;
        ch->warning_streak = current >= lim->warning_a ? streak_inc(ch->warning_streak) : 0U;
        ch->clear_streak = current < lim->warning_a * INA3221_LIMITS_CLEAR_RATIO
            ? streak_inc(ch->clear_streak) : 0U;

        if (ch->critical_streak >= INA3221_LIMITS_CONFIRM_SAMPLES) {
            if (!ch->latched) {
                ch->latched = true;
                ch->latched_current_a = s->current_a;
                newly_latched = true;
            }
            ch->level = INA3221_OVERCURRENT_CRITICAL;
            continue;
        }
        if (ch->latched) {
            ch->level = INA3221_OVERCURRENT_CRITICAL;
            continue;
        }
        if (ch->warning_streak >= INA3221_LIMITS_CONFIRM_SAMPLES) {
            ch->level = INA3221_OVERCURRENT_WARNING;
        } else if (ch->level != INA3221_OVERCURRENT_NONE &&
                   ch->clear_streak >= INA3221_LIMITS_CONFIRM_SAMPLES) {
            ch->level = INA3221_OVERCURRENT_NONE;
        }
    }
    return newly_latched;
}


void ina3221_overcurrent_clear_latch(
    ina3221_overcurrent_tracker_t *tracker,
    size_t channel)
{
    if (tracker == NULL || channel >= INA3221_POLICY_CHANNEL_COUNT) {
        return;
    }
    memset(&tracker->channels[channel], 0, sizeof(tracker->channels[channel]));
}


const char *ina3221_overcurrent_level_name(ina3221_overcurrent_level_t level)
{
    switch (level) {
        case INA3221_OVERCURRENT_NONE:
            return "none";
        case INA3221_OVERCURRENT_WARNING:
            return "warning";
        case INA3221_OVERCURRENT_CRITICAL:
            return "critical";
        default:
            return "unknown";
    }
}
