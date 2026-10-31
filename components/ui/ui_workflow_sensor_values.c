#include "ui_workflow_sensor_values.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool ui_workflow_status_hold_update(ui_workflow_status_hold_t *hold, bool read_ok, uint64_t now_ms)
{
    if (hold == NULL) return read_ok;
    if (read_ok) {
        hold->valid = true;
        hold->last_ok_ms = now_ms;
        return true;
    }
    return hold->valid && now_ms >= hold->last_ok_ms &&
        now_ms - hold->last_ok_ms < UI_WORKFLOW_STATUS_HOLD_MS;
}

bool ui_workflow_sensor_values_page(uint64_t now_ms)
{
    return ((now_ms / UI_WORKFLOW_SENSOR_PAGE_MS) & 1U) != 0U;
}

void ui_workflow_sensor_value_text(
    char *out,
    size_t size,
    bool valid,
    float value,
    unsigned decimals)
{
    if (out == NULL || size == 0U) return;
    if (!valid || !isfinite(value)) {
        (void)snprintf(out, size, "--");
        return;
    }
    if (decimals > 3U) decimals = 3U;
    (void)snprintf(out, size, "%.*f", (int)decimals, (double)value);
    /* A tiny negative value rounds to "-0.0": show it as plain zero. */
    if (out[0] == '-') {
        bool all_zero = true;
        for (const char *c = out + 1; *c != '\0'; ++c) {
            if (*c != '0' && *c != '.') {
                all_zero = false;
                break;
            }
        }
        if (all_zero) memmove(out, out + 1, strlen(out));
    }
}

void ui_workflow_sensor_pressure_text(
    char *out,
    size_t size,
    bool valid,
    float pressure_pa)
{
    ui_workflow_sensor_value_text(out, size, valid, pressure_pa / 100.0f, 1U);
}

void ui_workflow_sensor_values_line(
    char *out,
    size_t size,
    const char *node,
    bool pressure_valid,
    float pressure_pa,
    bool temperature_valid,
    float temperature_c,
    bool gyro_valid,
    float gyro_rate_dps)
{
    if (out == NULL || size == 0U) return;
    char pressure[12], temperature[12], gyro[12];
    ui_workflow_sensor_pressure_text(pressure, sizeof(pressure), pressure_valid, pressure_pa);
    ui_workflow_sensor_value_text(temperature, sizeof(temperature), temperature_valid, temperature_c, 1U);
    ui_workflow_sensor_value_text(gyro, sizeof(gyro), gyro_valid, gyro_rate_dps, 1U);
    (void)snprintf(out, size, "%s %s %s %s", node != NULL ? node : "", pressure, temperature, gyro);
}
