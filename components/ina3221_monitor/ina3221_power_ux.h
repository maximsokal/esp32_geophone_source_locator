#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ina3221_policy.h"

/*
 * MAX-180: user-facing power state on top of the source classifier
 * (ina3221_policy: USB / USB_CHARGING / BATTERY / UNKNOWN).
 *
 * Voltage thresholds apply to the battery SOURCE voltage (bus + shunt
 * drop) and are NOT calibrated: the pack topology and the UPS cutoff are
 * still unverified (MAX-184). They come from Kconfig with conservative
 * 1S Li-ion defaults and must be re-measured under realistic load.
 *
 * Stability: a new state is published only after it was computed on
 * `confirm` consecutive valid samples and a minimum dwell (CRITICAL needs
 * more, so one load spike cannot start a shutdown). LOW/CRITICAL and FULL
 * use separate enter/exit voltages (hysteresis), so the UI does not flicker.
 */

typedef enum
{
    INA3221_POWER_UX_UNKNOWN = 0,
    INA3221_POWER_UX_USB_ONLY_NO_BATTERY,
    INA3221_POWER_UX_USB_CHARGING,
    /* USB present, battery present, not charging (terminated or idle). */
    INA3221_POWER_UX_USB_BATTERY_READY,
    INA3221_POWER_UX_BATTERY,
    INA3221_POWER_UX_BATTERY_LOW,
    INA3221_POWER_UX_BATTERY_CRITICAL,

} ina3221_power_ux_state_t;

typedef struct
{
    float low_enter_v;
    float low_exit_v;
    float critical_enter_v;
    float critical_exit_v;
    /* USB_BATTERY_READY is reported full at full_v, and stays full to
     * full_exit_v. Both transitions use the regular confirmation dwell. */
    float full_v;
    float full_exit_v;
    uint8_t confirm_samples;
    uint8_t critical_confirm_samples;
    uint32_t confirm_dwell_ms;
    uint32_t critical_confirm_dwell_ms;

} ina3221_power_ux_config_t;

typedef struct
{
    ina3221_power_ux_state_t state;
    ina3221_power_ux_state_t candidate;
    uint8_t candidate_count;
    uint64_t candidate_since_ms;
    bool candidate_since_valid;
    uint64_t full_candidate_since_ms;
    uint8_t full_candidate_count;
    bool battery_full;
    bool full_candidate;
    bool full_candidate_valid;
    bool initialized;

} ina3221_power_ux_t;

typedef struct
{
    /* Stable source state from ina3221_policy_classifier_update(). */
    ina3221_power_state_t source;
    bool telemetry_valid;
    bool battery_present;
    float battery_voltage_v;

} ina3221_power_ux_input_t;

void ina3221_power_ux_default_config(ina3221_power_ux_config_t *config);

/* Enter < exit for both warning bands; FULL exit is above LOW exit and
 * below FULL entry. */
bool ina3221_power_ux_config_valid(const ina3221_power_ux_config_t *config);

void ina3221_power_ux_reset(ina3221_power_ux_t *ux);

/* The state this single sample points to, given the current stable state
 * (for hysteresis). Exposed for tests. */
ina3221_power_ux_state_t ina3221_power_ux_classify(
    const ina3221_power_ux_config_t *config,
    ina3221_power_ux_state_t current,
    const ina3221_power_ux_input_t *input);

/* Feed one sample; returns the stable published state. */
ina3221_power_ux_state_t ina3221_power_ux_update(
    ina3221_power_ux_t *ux,
    const ina3221_power_ux_config_t *config,
    const ina3221_power_ux_input_t *input,
    uint64_t now_ms);

bool ina3221_power_ux_usb_present(ina3221_power_ux_state_t state);

const char *ina3221_power_ux_state_name(ina3221_power_ux_state_t state);

/*
 * MAX-186: raw evidence whose changes are logged the moment they happen
 * (the periodic sample line is throttled). The first observation reports
 * nothing: there is no previous value to change from.
 */
typedef struct
{
    bool observed;
    bool battery_present;
    bool usb_present;
    bool charging;
    int soc_quality;

} ina3221_power_evidence_t;

#define INA3221_EVIDENCE_BATTERY_CHANGED  (1U << 0)
#define INA3221_EVIDENCE_USB_CHANGED      (1U << 1)
#define INA3221_EVIDENCE_CHARGING_CHANGED (1U << 2)
#define INA3221_EVIDENCE_SOC_CHANGED      (1U << 3)

uint32_t ina3221_power_evidence_changes(
    const ina3221_power_evidence_t *previous,
    const ina3221_power_evidence_t *current);
