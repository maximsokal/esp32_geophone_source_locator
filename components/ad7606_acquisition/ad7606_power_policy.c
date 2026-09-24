#include "ad7606_power_policy.h"

#include <math.h>
#include <stddef.h>


static bool reading_fresh(bool available, bool valid, float voltage_v, int64_t sampled_at_us, int64_t now_us)
{
    return
        available &&
        valid &&
        isfinite(voltage_v) &&
        sampled_at_us > 0 &&
        now_us >= sampled_at_us &&
        now_us - sampled_at_us <= AD7606_RAIL_MAX_AGE_US;
}


ad7606_power_gate_t ad7606_power_gate(
    bool reading_available,
    bool reading_valid,
    float rail_voltage_v,
    int64_t sampled_at_us,
    int64_t now_us
)
{
    if (!reading_fresh(reading_available, reading_valid, rail_voltage_v, sampled_at_us, now_us)) {
        return AD7606_POWER_GATE_UNVERIFIED;
    }
    return rail_voltage_v >= AD7606_RAIL_START_MIN_V
        ? AD7606_POWER_GATE_OK
        : AD7606_POWER_GATE_RAIL_LOW;
}


void ad7606_rail_watch_reset(ad7606_rail_watch_t *watch)
{
    if (watch != NULL) {
        watch->low_samples = 0U;
        watch->last_sampled_at_us = 0;
    }
}


bool ad7606_rail_watch_update(
    ad7606_rail_watch_t *watch,
    bool reading_available,
    bool reading_valid,
    float rail_voltage_v,
    int64_t sampled_at_us,
    int64_t now_us
)
{
    if (watch == NULL) {
        return false;
    }
    if (!reading_fresh(reading_available, reading_valid, rail_voltage_v, sampled_at_us, now_us)) {
        return false;
    }
    if (sampled_at_us == watch->last_sampled_at_us) {
        return false; /* already counted this INA3221 conversion */
    }
    watch->last_sampled_at_us = sampled_at_us;

    if (rail_voltage_v < AD7606_RAIL_LOSS_V) {
        if (watch->low_samples < 255U) {
            ++watch->low_samples;
        }
    } else {
        watch->low_samples = 0U;
    }
    return watch->low_samples >= AD7606_RAIL_LOSS_CONFIRM_SAMPLES;
}


const char *ad7606_power_gate_name(ad7606_power_gate_t gate)
{
    switch (gate) {
        case AD7606_POWER_GATE_OK:
            return "ok";
        case AD7606_POWER_GATE_UNVERIFIED:
            return "unverified";
        case AD7606_POWER_GATE_RAIL_LOW:
            return "rail_low";
        case AD7606_POWER_GATE_OVERCURRENT:
            return "overcurrent";
        default:
            return "unknown";
    }
}
