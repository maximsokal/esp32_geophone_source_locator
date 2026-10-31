#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The sensor-check cards alternate between link status and measured values. */
#define UI_WORKFLOW_SENSOR_PAGE_MS 3000U

/*
 * The cards read the shared device status without waiting (a blocked LVGL task freezes the display),
 * so a read can fail only because a lower-priority writer was preempted while holding the lock. How
 * long the last good snapshot may stand in for failed reads before the cards are blanked.
 */
#define UI_WORKFLOW_STATUS_HOLD_MS 1500U

typedef struct {
    bool valid;
    uint64_t last_ok_ms;
} ui_workflow_status_hold_t;

/*
 * Call once per refresh with the outcome of the non-blocking read. Returns true when the snapshot
 * the caller holds may still be used (this read succeeded, or the last good one is younger than
 * UI_WORKFLOW_STATUS_HOLD_MS); false when it must be cleared.
 */
bool ui_workflow_status_hold_update(ui_workflow_status_hold_t *hold, bool read_ok, uint64_t now_ms);

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
