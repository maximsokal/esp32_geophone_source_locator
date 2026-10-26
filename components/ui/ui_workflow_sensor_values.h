#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The sensor-check cards alternate between link status and measured values. */
#define UI_WORKFLOW_SENSOR_PAGE_MS 3000U

/* True while the measured-values page is shown, false for the status page. */
bool ui_workflow_sensor_values_page(uint64_t now_ms);

/* "%.<decimals>f", or "--" when not valid. Never prints "-0.0". */
void ui_workflow_sensor_value_text(
    char *out,
    size_t size,
    bool valid,
    float value,
    unsigned decimals);

/* Pressure in hPa with one decimal, or "--". */
void ui_workflow_sensor_pressure_text(
    char *out,
    size_t size,
    bool valid,
    float pressure_pa);

/*
 * One card line of measured values: "<node> <hPa> <degC> <gyro deg/s>",
 * e.g. "G1 1013.2 24.1 0.3". Missing values print as "--". The units are
 * explained once in the footer, so the line stays short enough for the card.
 */
void ui_workflow_sensor_values_line(
    char *out,
    size_t size,
    const char *node,
    bool pressure_valid,
    float pressure_pa,
    bool temperature_valid,
    float temperature_c,
    bool gyro_valid,
    float gyro_rate_dps);
