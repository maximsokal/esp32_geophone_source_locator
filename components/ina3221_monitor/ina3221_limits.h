#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ina3221_policy.h"

/*
 * MAX-245: INA3221 over-current limits.
 *
 * Two layers:
 *  - hardware: the Critical/Warning alert limit registers (CH1 0x07/0x08,
 *    CH2 0x09/0x0A, CH3 0x0B/0x0C). CRI/WAR are NOT wired on this board,
 *    so the chip's comparators are only observable in software and are
 *    never a hardware cutoff. They compare the SIGNED shunt voltage
 *    against a positive limit; the two 5 V rail shunts are installed
 *    load->source, so their load current reads negative and a positive
 *    limit could never trip. Only CH1 (battery, discharge positive) is
 *    programmed; CH2/CH3 are set to the maximum (comparator disabled).
 *  - software: the reaction uses the sign-corrected current of every
 *    channel (ina3221_channel_sample_t.current_a) with confirmation,
 *    hysteresis and a latch.
 *
 * The default limits are engineering estimates, NOT measured values; they
 * stay below the 163.8 mV shunt full scale (1.638 A at 0.1 Ohm) and must be
 * calibrated on the bench (HIL).
 */

#define INA3221_LIMITS_REG_CRITICAL_CH1 0x07U
#define INA3221_LIMITS_REG_WARNING_CH1  0x08U
#define INA3221_LIMITS_REG_DISABLED     0x7FF8U

/* Consecutive valid samples needed to enter a level. */
#define INA3221_LIMITS_CONFIRM_SAMPLES 2U
/* A warning clears below this fraction of its threshold. */
#define INA3221_LIMITS_CLEAR_RATIO 0.9f

typedef enum
{
    INA3221_OVERCURRENT_NONE = 0,
    INA3221_OVERCURRENT_WARNING,
    INA3221_OVERCURRENT_CRITICAL,

} ina3221_overcurrent_level_t;

typedef struct
{
    float warning_a;
    float critical_a;
    /* Program this channel's hardware alert limits (see above). */
    bool program_hardware;

} ina3221_channel_limit_t;

typedef struct
{
    ina3221_channel_limit_t channels[INA3221_POLICY_CHANNEL_COUNT];

} ina3221_limits_t;

typedef struct
{
    ina3221_overcurrent_level_t level;
    /* A confirmed CRITICAL stays latched until ina3221_limits_clear_latch. */
    bool latched;
    float latched_current_a;
    uint8_t warning_streak;
    uint8_t critical_streak;
    uint8_t clear_streak;

} ina3221_overcurrent_channel_t;

typedef struct
{
    ina3221_overcurrent_channel_t channels[INA3221_POLICY_CHANNEL_COUNT];

} ina3221_overcurrent_tracker_t;

/* Project defaults (Kconfig can override, see ina3221_monitor.c). */
void ina3221_limits_default(ina3221_limits_t *limits);

/* Thresholds must be positive, warning < critical, and inside the shunt
 * full scale; otherwise the limits are rejected. */
bool ina3221_limits_valid(const ina3221_limits_t *limits, float shunt_ohm);

/* Alert-limit register value for a current through `shunt_ohm`:
 * signed 13-bit shunt voltage (LSB 40 uV) in bits 15..3, clamped. */
uint16_t ina3221_limits_register_value(float current_a, float shunt_ohm);

/* Register values to write for one channel (DISABLED when not programmed). */
void ina3221_limits_channel_registers(
    const ina3221_limits_t *limits,
    size_t channel,
    float shunt_ohm,
    uint16_t *critical_reg,
    uint16_t *warning_reg);

void ina3221_overcurrent_reset(ina3221_overcurrent_tracker_t *tracker);

/* Feed one sample set. Invalid channels neither confirm nor clear. Returns
 * true when a channel newly latched CRITICAL on this sample. */
bool ina3221_overcurrent_update(
    ina3221_overcurrent_tracker_t *tracker,
    const ina3221_limits_t *limits,
    const ina3221_channel_sample_t samples[INA3221_POLICY_CHANNEL_COUNT]);

void ina3221_overcurrent_clear_latch(
    ina3221_overcurrent_tracker_t *tracker,
    size_t channel);

const char *ina3221_overcurrent_level_name(ina3221_overcurrent_level_t level);
