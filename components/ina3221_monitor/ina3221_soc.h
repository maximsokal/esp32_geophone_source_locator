#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * MAX-181: hybrid battery state-of-charge estimator.
 *
 *  - Anchor from the voltage curve only when the pack is at rest (|I| small),
 *    because load/charge current shifts the terminal voltage.
 *  - Between anchors, integrate the measured battery current
 *    (positive = discharge): dQ_mAh = -I_A * dt_s * 1000 / 3600.
 *  - Re-anchor to 100 % at a proven full charge (USB present, charge
 *    terminated, voltage at/above "full") and from the curve after a long
 *    rest.
 *  - A gap between samples (missing INA3221 data, stopped monitor, reboot)
 *    is never integrated as zero current: tracking is dropped to
 *    VOLTAGE_ONLY until the next anchor.
 *  - Capacity 0 = unknown (pack topology not verified, MAX-184): the
 *    estimator stays VOLTAGE_ONLY and never pretends to count coulombs.
 *
 * Nothing is persisted across reboots: after a reboot the charge that flowed
 * while the device was off is unknown, so the estimate restarts from the
 * voltage curve.
 */

typedef enum
{
    INA3221_SOC_INVALID = 0,
    /* Terminal-voltage curve only (no capacity, or tracking lost). */
    INA3221_SOC_VOLTAGE_ONLY,
    /* Integrated since a rest-voltage anchor. */
    INA3221_SOC_COULOMB_TRACKED,
    /* Integrated since a proven full charge. */
    INA3221_SOC_FULL_ANCHORED,

} ina3221_soc_quality_t;

typedef struct
{
    float capacity_mah;        /* 0 = unknown */
    float rest_current_a;      /* |I| below this counts as rest */
    uint32_t rest_anchor_ms;   /* rest needed to re-anchor from voltage */
    uint32_t max_gap_ms;       /* larger sample gaps drop tracking */
    float full_v;

} ina3221_soc_config_t;

typedef struct
{
    ina3221_soc_quality_t quality;
    float charge_mah;          /* remaining charge while tracked */
    float voltage_percent;     /* last curve estimate */
    int64_t last_sample_ms;
    int64_t rest_since_ms;     /* -1 = not resting */
    bool have_sample;

} ina3221_soc_t;

typedef struct
{
    bool valid;                /* battery present and telemetry valid */
    float voltage_v;           /* battery source voltage */
    float current_a;           /* + discharge, - charge */
    bool usb_present;
    bool charging;

} ina3221_soc_input_t;

void ina3221_soc_default_config(ina3221_soc_config_t *config);
void ina3221_soc_reset(ina3221_soc_t *soc);

/* Feed one sample taken at now_ms. */
void ina3221_soc_update(
    ina3221_soc_t *soc,
    const ina3221_soc_config_t *config,
    const ina3221_soc_input_t *input,
    int64_t now_ms);

/* 0..100; from the tracked charge when tracking, else the voltage curve. */
float ina3221_soc_percent(const ina3221_soc_t *soc, const ina3221_soc_config_t *config);

const char *ina3221_soc_quality_name(ina3221_soc_quality_t quality);

/* The 1S Li-ion open-circuit curve used for anchoring (0..100 %). */
float ina3221_soc_voltage_percent(float voltage_v);
