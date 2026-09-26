#include "ina3221_power_ux.h"

#include <math.h>
#include <stddef.h>
#include <string.h>


void ina3221_power_ux_default_config(ina3221_power_ux_config_t *config)
{
    if (config == NULL) {
        return;
    }
    /* Uncalibrated 1S Li-ion source-voltage defaults (see header). */
    *config = (ina3221_power_ux_config_t) {
        .low_enter_v = 3.45f,
        .low_exit_v = 3.60f,
        .critical_enter_v = 3.30f,
        .critical_exit_v = 3.45f,
        .full_v = 4.10f,
        .full_exit_v = 4.05f,
        .confirm_samples = 3U,
        .critical_confirm_samples = 5U,
        .confirm_dwell_ms = 2000U,
        .critical_confirm_dwell_ms = 4000U,
    };
}


bool ina3221_power_ux_config_valid(const ina3221_power_ux_config_t *config)
{
    return config != NULL &&
        isfinite(config->low_enter_v) && isfinite(config->full_v) &&
        isfinite(config->full_exit_v) && config->full_exit_v > config->low_exit_v &&
        config->full_exit_v < config->full_v &&
        config->critical_enter_v > 0.0f &&
        config->critical_enter_v < config->critical_exit_v &&
        config->low_enter_v < config->low_exit_v &&
        config->critical_enter_v < config->low_enter_v &&
        config->critical_exit_v <= config->low_exit_v &&
        config->full_v > config->low_exit_v &&
        config->confirm_samples >= 1U &&
        config->critical_confirm_samples >= config->confirm_samples &&
        config->critical_confirm_dwell_ms >= config->confirm_dwell_ms;
}


void ina3221_power_ux_reset(ina3221_power_ux_t *ux)
{
    if (ux != NULL) {
        memset(ux, 0, sizeof(*ux));
    }
}


static bool is_battery_band(ina3221_power_ux_state_t s)
{
    return s == INA3221_POWER_UX_BATTERY ||
        s == INA3221_POWER_UX_BATTERY_LOW ||
        s == INA3221_POWER_UX_BATTERY_CRITICAL;
}


static void update_full_state(
    ina3221_power_ux_t *ux,
    const ina3221_power_ux_config_t *config,
    const ina3221_power_ux_input_t *input,
    uint64_t now_ms)
{
    const bool voltage_full = ux->battery_full
        ? input->battery_voltage_v > config->full_exit_v
        : input->battery_voltage_v >= config->full_v;
    const bool candidate = ux->state == INA3221_POWER_UX_USB_BATTERY_READY &&
        input->source == INA3221_POWER_STATE_USB && input->battery_present &&
        isfinite(input->battery_voltage_v) && voltage_full;

    if (candidate == ux->battery_full) {
        ux->full_candidate = candidate;
        ux->full_candidate_count = 0U;
        ux->full_candidate_valid = false;
        return;
    }
    if (!ux->full_candidate_valid || candidate != ux->full_candidate ||
        now_ms < ux->full_candidate_since_ms) {
        ux->full_candidate = candidate;
        ux->full_candidate_count = 1U;
        ux->full_candidate_since_ms = now_ms;
        ux->full_candidate_valid = true;
    } else if (ux->full_candidate_count < UINT8_MAX) {
        ux->full_candidate_count++;
    }

    if (ux->full_candidate_count >= config->confirm_samples &&
        now_ms - ux->full_candidate_since_ms >= config->confirm_dwell_ms) {
        ux->battery_full = candidate;
        ux->full_candidate_count = 0U;
        ux->full_candidate_valid = false;
    }
}


ina3221_power_ux_state_t ina3221_power_ux_classify(
    const ina3221_power_ux_config_t *config,
    ina3221_power_ux_state_t current,
    const ina3221_power_ux_input_t *input)
{
    if (config == NULL || input == NULL || !input->telemetry_valid) {
        return INA3221_POWER_UX_UNKNOWN;
    }

    switch (input->source) {
        case INA3221_POWER_STATE_USB:
        case INA3221_POWER_STATE_USB_CHARGING:
            if (!input->battery_present) {
                return INA3221_POWER_UX_USB_ONLY_NO_BATTERY;
            }
            return input->source == INA3221_POWER_STATE_USB_CHARGING
                ? INA3221_POWER_UX_USB_CHARGING
                : INA3221_POWER_UX_USB_BATTERY_READY;

        case INA3221_POWER_STATE_BATTERY: {
            if (!input->battery_present || !isfinite(input->battery_voltage_v)) {
                return INA3221_POWER_UX_UNKNOWN;
            }
            const float v = input->battery_voltage_v;
            /* Hysteresis: leaving a lower band needs the higher exit voltage. */
            if (current == INA3221_POWER_UX_BATTERY_CRITICAL) {
                if (v < config->critical_exit_v) {
                    return INA3221_POWER_UX_BATTERY_CRITICAL;
                }
                return v < config->low_exit_v
                    ? INA3221_POWER_UX_BATTERY_LOW
                    : INA3221_POWER_UX_BATTERY;
            }
            if (v < config->critical_enter_v) {
                return INA3221_POWER_UX_BATTERY_CRITICAL;
            }
            if (current == INA3221_POWER_UX_BATTERY_LOW) {
                return v < config->low_exit_v
                    ? INA3221_POWER_UX_BATTERY_LOW
                    : INA3221_POWER_UX_BATTERY;
            }
            return v < config->low_enter_v
                ? INA3221_POWER_UX_BATTERY_LOW
                : INA3221_POWER_UX_BATTERY;
        }

        case INA3221_POWER_STATE_UNKNOWN:
        default:
            return INA3221_POWER_UX_UNKNOWN;
    }
}


ina3221_power_ux_state_t ina3221_power_ux_update(
    ina3221_power_ux_t *ux,
    const ina3221_power_ux_config_t *config,
    const ina3221_power_ux_input_t *input,
    uint64_t now_ms)
{
    if (ux == NULL || config == NULL) {
        return INA3221_POWER_UX_UNKNOWN;
    }

    if (input == NULL || !input->telemetry_valid ||
        input->source == INA3221_POWER_STATE_UNKNOWN) {
        ux->initialized = true;
        ux->state = INA3221_POWER_UX_UNKNOWN;
        ux->candidate = INA3221_POWER_UX_UNKNOWN;
        ux->candidate_count = 0U;
        ux->candidate_since_valid = false;
        ux->battery_full = false;
        ux->full_candidate = false;
        ux->full_candidate_count = 0U;
        ux->full_candidate_valid = false;
        return ux->state;
    }

    const ina3221_power_ux_state_t instant =
        ina3221_power_ux_classify(config, ux->state, input);

    if (instant == INA3221_POWER_UX_UNKNOWN) {
        ux->initialized = true;
        ux->state = INA3221_POWER_UX_UNKNOWN;
        ux->candidate = INA3221_POWER_UX_UNKNOWN;
        ux->candidate_count = 0U;
        ux->candidate_since_valid = false;
        ux->battery_full = false;
        ux->full_candidate = false;
        ux->full_candidate_count = 0U;
        ux->full_candidate_valid = false;
        return ux->state;
    }

    if (!ux->initialized) {
        /* Never publish a battery warning/critical from a single reading:
         * start from the non-alarming state and let the confirmation below
         * promote it (this first sample counts once). */
        ux->initialized = true;
        ux->state = is_battery_band(instant) ? INA3221_POWER_UX_BATTERY : instant;
        ux->candidate = ux->state;
        ux->candidate_count = 0U;
        ux->candidate_since_valid = false;
    }

    if (instant == ux->state) {
        ux->candidate = instant;
        ux->candidate_count = 0U;
        ux->candidate_since_valid = false;
        update_full_state(ux, config, input, now_ms);
        return ux->state;
    }
    if (instant != ux->candidate) {
        ux->candidate = instant;
        ux->candidate_count = 1U;
        ux->candidate_since_ms = now_ms;
        ux->candidate_since_valid = true;
    } else if (ux->candidate_count < UINT8_MAX) {
        ux->candidate_count++;
    }

    if (!ux->candidate_since_valid || now_ms < ux->candidate_since_ms) {
        ux->candidate_since_ms = now_ms;
        ux->candidate_since_valid = true;
        ux->candidate_count = 1U;
    }

    const uint8_t needed = ux->candidate == INA3221_POWER_UX_BATTERY_CRITICAL
        ? config->critical_confirm_samples
        : config->confirm_samples;
    const uint32_t dwell_ms = ux->candidate == INA3221_POWER_UX_BATTERY_CRITICAL
        ? config->critical_confirm_dwell_ms
        : config->confirm_dwell_ms;
    if (ux->candidate_count >= needed && now_ms - ux->candidate_since_ms >= dwell_ms) {
        ux->state = ux->candidate;
        ux->candidate_count = 0U;
        ux->candidate_since_valid = false;
    }
    update_full_state(ux, config, input, now_ms);
    return ux->state;
}


bool ina3221_power_ux_usb_present(ina3221_power_ux_state_t state)
{
    return state == INA3221_POWER_UX_USB_ONLY_NO_BATTERY ||
        state == INA3221_POWER_UX_USB_CHARGING ||
        state == INA3221_POWER_UX_USB_BATTERY_READY;
}


const char *ina3221_power_ux_state_name(ina3221_power_ux_state_t state)
{
    switch (state) {
        case INA3221_POWER_UX_USB_ONLY_NO_BATTERY: return "USB_ONLY_NO_BATTERY";
        case INA3221_POWER_UX_USB_CHARGING: return "USB_CHARGING";
        case INA3221_POWER_UX_USB_BATTERY_READY: return "USB_BATTERY_READY";
        case INA3221_POWER_UX_BATTERY: return "BATTERY";
        case INA3221_POWER_UX_BATTERY_LOW: return "BATTERY_LOW";
        case INA3221_POWER_UX_BATTERY_CRITICAL: return "BATTERY_CRITICAL";
        case INA3221_POWER_UX_UNKNOWN:
        default: return "UNKNOWN";
    }
}


uint32_t ina3221_power_evidence_changes(
    const ina3221_power_evidence_t *previous,
    const ina3221_power_evidence_t *current)
{
    if (previous == NULL || current == NULL || !previous->observed || !current->observed) {
        return 0U;
    }
    uint32_t changes = 0U;
    if (previous->battery_present != current->battery_present) {
        changes |= INA3221_EVIDENCE_BATTERY_CHANGED;
    }
    if (previous->usb_present != current->usb_present) {
        changes |= INA3221_EVIDENCE_USB_CHANGED;
    }
    if (previous->charging != current->charging) {
        changes |= INA3221_EVIDENCE_CHARGING_CHANGED;
    }
    if (previous->soc_quality != current->soc_quality) {
        changes |= INA3221_EVIDENCE_SOC_CHANGED;
    }
    return changes;
}
