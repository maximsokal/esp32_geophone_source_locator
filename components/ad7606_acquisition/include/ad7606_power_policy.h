#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * MAX-242: firmware power interlock for the AD7606 -- the SECOND layer on
 * top of the hardware fix (AD7606 VIO from its own LD1117V33 on the AD7606
 * +5 V rail, CS pull-up on that VIO). Pure policy, host-tested; the caller
 * supplies the INA3221 reading of the AD7606 +5 V rail (physical CH3).
 */

/* AD7606 AVCC recommended minimum (datasheet: 4.75 .. 5.25 V). */
#define AD7606_RAIL_START_MIN_V 4.75f

/* Below this the running AD7606 is treated as unpowered. Lower than the
 * start threshold (hysteresis) so normal load ripple never trips it. */
#define AD7606_RAIL_LOSS_V 4.50f

/* A reading older than this proves nothing about the rail now. */
#define AD7606_RAIL_MAX_AGE_US 3000000LL

/* Consecutive fresh low readings required before acting (one glitchy
 * INA3221 conversion must not stop a recording). */
#define AD7606_RAIL_LOSS_CONFIRM_SAMPLES 2U

typedef enum
{
    /* A fresh, valid reading shows the rail within the start range. */
    AD7606_POWER_GATE_OK = 0,
    /* No fresh valid reading (INA3221 absent, faulted, not sampled yet or
     * stale): start is allowed with a warning -- the hardware LD1117 layer
     * already prevents the dangerous VIO-without-AVCC state. */
    AD7606_POWER_GATE_UNVERIFIED,
    /* A fresh, valid reading shows the rail below the start threshold:
     * never drive any AD7606 pin. */
    AD7606_POWER_GATE_RAIL_LOW,
    /* MAX-245: the INA3221 monitor latched a critical over-current on the
     * AD7606 +5 V rail while it was running. */
    AD7606_POWER_GATE_OVERCURRENT,

} ad7606_power_gate_t;

ad7606_power_gate_t ad7606_power_gate(
    bool reading_available,
    bool reading_valid,
    float rail_voltage_v,
    int64_t sampled_at_us,
    int64_t now_us
);

typedef struct
{
    uint8_t low_samples;
    int64_t last_sampled_at_us;

} ad7606_rail_watch_t;

void ad7606_rail_watch_reset(ad7606_rail_watch_t *watch);

/*
 * Feeds one INA3221 reading while the AD7606 runs. Returns true exactly when
 * rail loss is confirmed: AD7606_RAIL_LOSS_CONFIRM_SAMPLES distinct fresh
 * valid readings below AD7606_RAIL_LOSS_V in a row. The same reading seen
 * twice counts once; invalid or stale readings neither count nor clear the
 * streak (the rail state is unknown, not good).
 */
bool ad7606_rail_watch_update(
    ad7606_rail_watch_t *watch,
    bool reading_available,
    bool reading_valid,
    float rail_voltage_v,
    int64_t sampled_at_us,
    int64_t now_us
);

const char *ad7606_power_gate_name(ad7606_power_gate_t gate);
