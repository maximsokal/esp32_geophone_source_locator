#pragma once

#include "esp_err.h"
#include "i2c_line_recovery.h"

/*
 * MAX-243: what the topology worker does after a failed upstream transfer.
 *
 * ESP-IDF 5.5 i2c_master reports a target NACK and a hardware timeout both
 * as ESP_ERR_INVALID_STATE. Callers first classify the error with
 * sensor_bus_classify_transfer_error() (MAX-248), so a plain NACK arrives
 * here as ESP_ERR_NOT_FOUND and is never checked. What remains is decided
 * by the line check inside `recover` (sensor_bus_recover):
 *  - lines idle      -> transient; the mux is left alone;
 *  - SDA cleared     -> close the mux (tca9548a_mux_close reads the control
 *                       register back) so no half-selected channel remains;
 *  - line still stuck-> fault, the mux is not touched (the bus cannot carry
 *                       the close), ESP_ERR_TIMEOUT;
 *  - backoff refused -> nothing is touched.
 * NACK codes of the legacy driver (ESP_FAIL) never trigger a line check.
 */
typedef struct
{
    esp_err_t (*recover)(void *context, i2c_line_result_t *result);
    esp_err_t (*mux_close)(void *context);
    void *context;

} nrf_topology_bus_recovery_ops_t;

typedef enum
{
    NRF_TOPOLOGY_BUS_NOT_CHECKED = 0,
    NRF_TOPOLOGY_BUS_BACKOFF,
    NRF_TOPOLOGY_BUS_LINES_IDLE,
    NRF_TOPOLOGY_BUS_RECOVERED_MUX_CLOSED,
    NRF_TOPOLOGY_BUS_RECOVERED_MUX_FAULT,
    NRF_TOPOLOGY_BUS_STUCK,

} nrf_topology_bus_recovery_outcome_t;

/* Returns the error the caller should propagate. */
esp_err_t nrf_topology_bus_recover_after_error(
    esp_err_t transfer_err,
    const nrf_topology_bus_recovery_ops_t *ops,
    nrf_topology_bus_recovery_outcome_t *outcome
);
