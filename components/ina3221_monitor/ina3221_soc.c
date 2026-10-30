#include "ina3221_soc.h"

#include <math.h>
#include <stddef.h>
#include <string.h>


typedef struct
{
    float voltage_v;
    float percent;

} soc_curve_point_t;

/* Approximate 1S Li-ion rest curve; to be replaced by a measured one (MAX-184). */
static const soc_curve_point_t k_curve[] = {
    { 3.00f,   0.0f },
    { 3.30f,   2.0f },
    { 3.50f,   7.0f },
    { 3.60f,  15.0f },
    { 3.70f,  35.0f },
    { 3.80f,  55.0f },
    { 3.90f,  70.0f },
    { 4.00f,  80.0f },
    { 4.10f,  90.0f },
    { 4.20f, 100.0f },
};


float ina3221_soc_voltage_percent(float voltage_v)
{
    const size_t count = sizeof(k_curve) / sizeof(k_curve[0]);
    if (!isfinite(voltage_v)) {
        return NAN;
    }
    if (voltage_v <= k_curve[0].voltage_v) {
        return 0.0f;
    }
    if (voltage_v >= k_curve[count - 1U].voltage_v) {
        return 100.0f;
    }
    for (size_t i = 1U; i < count; ++i) {
        if (voltage_v <= k_curve[i].voltage_v) {
            const float t = (voltage_v - k_curve[i - 1U].voltage_v) /
                (k_curve[i].voltage_v - k_curve[i - 1U].voltage_v);
            return k_curve[i - 1U].percent + (k_curve[i].percent - k_curve[i - 1U].percent) * t;
        }
    }
    return 100.0f;
}


void ina3221_soc_default_config(ina3221_soc_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (ina3221_soc_config_t) {
        .capacity_mah = 0.0f,          /* unknown until MAX-184 */
        .rest_current_a = 0.050f,
        .rest_anchor_ms = 30U * 60U * 1000U,
        .max_gap_ms = 10000U,  /* same window as INA3221_MONITOR_STALE_LIMIT_MS */
        .full_v = 4.10f,
    };
}


void ina3221_soc_reset(ina3221_soc_t *soc)
{
    if (soc != NULL) {
        memset(soc, 0, sizeof(*soc));
        soc->rest_since_ms = -1;
    }
}


static bool tracking(const ina3221_soc_t *soc)
{
    return soc->quality == INA3221_SOC_COULOMB_TRACKED ||
        soc->quality == INA3221_SOC_FULL_ANCHORED;
}


static void anchor(ina3221_soc_t *soc, const ina3221_soc_config_t *config, float percent, ina3221_soc_quality_t quality)
{
    if (config->capacity_mah > 0.0f) {
        soc->charge_mah = config->capacity_mah * percent / 100.0f;
        soc->quality = quality;
    } else {
        soc->quality = INA3221_SOC_VOLTAGE_ONLY;
    }
}


void ina3221_soc_update(
    ina3221_soc_t *soc,
    const ina3221_soc_config_t *config,
    const ina3221_soc_input_t *input,
    int64_t now_ms)
{
    if (soc == NULL || config == NULL || input == NULL) {
        return;
    }
    if (!input->valid || !isfinite(input->voltage_v) || !isfinite(input->current_a)) {
        /* Drop the estimate's validity and never integrate across a hole. */
        soc->quality = INA3221_SOC_INVALID;
        soc->have_sample = false;
        soc->rest_since_ms = -1;
        return;
    }

    soc->voltage_percent = ina3221_soc_voltage_percent(input->voltage_v);

    /* Integrate the interval since the previous valid sample. */
    if (soc->have_sample && tracking(soc)) {
        const int64_t dt_ms = now_ms - soc->last_sample_ms;
        if (dt_ms < 0 || dt_ms > (int64_t)config->max_gap_ms) {
            soc->quality = INA3221_SOC_VOLTAGE_ONLY; /* unknown charge flowed */
        } else {
            soc->charge_mah -= input->current_a * (float)dt_ms / 3600.0f;
            if (soc->charge_mah < 0.0f) {
                soc->charge_mah = 0.0f;
            }
            if (soc->charge_mah > config->capacity_mah) {
                soc->charge_mah = config->capacity_mah;
            }
        }
    }

    /* Rest tracking. */
    const bool resting = fabsf(input->current_a) < config->rest_current_a;
    if (!resting) {
        soc->rest_since_ms = -1;
    } else if (soc->rest_since_ms < 0) {
        soc->rest_since_ms = now_ms;
    }

    /* Anchors, strongest first. */
    if (input->usb_present && !input->charging && resting && input->voltage_v >= config->full_v) {
        anchor(soc, config, 100.0f, INA3221_SOC_FULL_ANCHORED);
    } else if (!tracking(soc)) {
        /* Bootstrap / recover only from a rest voltage. */
        if (resting) {
            anchor(soc, config, soc->voltage_percent, INA3221_SOC_COULOMB_TRACKED);
        } else {
            soc->quality = INA3221_SOC_VOLTAGE_ONLY;
        }
    } else if (resting && now_ms - soc->rest_since_ms >= (int64_t)config->rest_anchor_ms &&
               soc->quality != INA3221_SOC_FULL_ANCHORED) {
        /* Long rest: correct integration drift from the curve. */
        anchor(soc, config, soc->voltage_percent, INA3221_SOC_COULOMB_TRACKED);
        soc->rest_since_ms = now_ms;
    }

    soc->last_sample_ms = now_ms;
    soc->have_sample = true;
}


float ina3221_soc_percent(const ina3221_soc_t *soc, const ina3221_soc_config_t *config)
{
    if (soc == NULL || config == NULL || soc->quality == INA3221_SOC_INVALID || !soc->have_sample) {
        return NAN;
    }
    if (tracking(soc) && config->capacity_mah > 0.0f) {
        return 100.0f * soc->charge_mah / config->capacity_mah;
    }
    return soc->voltage_percent;
}


const char *ina3221_soc_quality_name(ina3221_soc_quality_t quality)
{
    switch (quality) {
        case INA3221_SOC_VOLTAGE_ONLY: return "VOLTAGE_ONLY";
        case INA3221_SOC_COULOMB_TRACKED: return "COULOMB_TRACKED";
        case INA3221_SOC_FULL_ANCHORED: return "FULL_ANCHORED";
        case INA3221_SOC_INVALID:
        default: return "INVALID";
    }
}
