#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#define INA3221_POLICY_CHANNEL_COUNT   3
#define INA3221_POLICY_STABLE_SAMPLES  3

/* Array indices are the physical INA3221 channels, not display channels. */
#define INA3221_POLICY_BATTERY_INDEX   0U  /* physical INA CH1 */
#define INA3221_POLICY_ESP32_INDEX     1U  /* physical INA CH2 */
#define INA3221_POLICY_AD7606_INDEX    2U  /* physical INA CH3 */


typedef enum
{
    INA3221_POWER_STATE_UNKNOWN = 0,
    INA3221_POWER_STATE_BATTERY,
    INA3221_POWER_STATE_USB,
    INA3221_POWER_STATE_USB_CHARGING,

} ina3221_power_state_t;


typedef struct
{
    bool valid;

    float bus_voltage_v;
    float shunt_voltage_v;
    float current_a;

} ina3221_channel_sample_t;


typedef struct
{
    bool initialized;

    ina3221_power_state_t stable_state;
    ina3221_power_state_t candidate_state;

    uint8_t candidate_count;

} ina3221_power_classifier_t;


typedef struct
{
    ina3221_power_state_t state;

    bool battery_present;
    bool battery_percent_valid;

    float battery_voltage_v;
    float battery_current_a;
    float battery_percent_estimate;

    bool usb_present;
    bool charging;

} ina3221_power_decision_t;


float ina3221_policy_bus_raw_to_volts(
    uint16_t raw
);

float ina3221_policy_shunt_raw_to_volts(
    uint16_t raw
);

float ina3221_policy_current_from_shunt(
    float shunt_voltage_v,
    float shunt_resistance_ohm
);

/* Apply the installed shunt orientation for a physical INA3221 channel. */
float ina3221_policy_current_for_channel(
    size_t channel_index,
    float shunt_voltage_v,
    float shunt_resistance_ohm
);

const char *ina3221_policy_channel_name(
    size_t channel_index
);

float ina3221_policy_battery_source_voltage(
    const ina3221_channel_sample_t *battery
);

bool ina3221_policy_battery_present(
    float battery_voltage_v
);

float ina3221_policy_battery_percent(
    float battery_voltage_v
);

ina3221_power_state_t ina3221_policy_classify_instant(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT]
);

ina3221_power_state_t ina3221_policy_classifier_update(
    ina3221_power_classifier_t *classifier,
    ina3221_power_state_t instantaneous_state
);

void ina3221_policy_make_decision(
    const ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT],
    ina3221_power_state_t stable_state,
    ina3221_power_decision_t *decision
);

const char *ina3221_policy_state_name(
    ina3221_power_state_t state
);
