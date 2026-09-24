#include "nrf_topology_bus_recovery.h"

#include <stddef.h>


static bool is_bus_level_error(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE;
}


esp_err_t nrf_topology_bus_recover_after_error(
    esp_err_t transfer_err,
    const nrf_topology_bus_recovery_ops_t *ops,
    nrf_topology_bus_recovery_outcome_t *outcome
)
{
    nrf_topology_bus_recovery_outcome_t local = NRF_TOPOLOGY_BUS_NOT_CHECKED;
    nrf_topology_bus_recovery_outcome_t *out = outcome != NULL ? outcome : &local;
    *out = NRF_TOPOLOGY_BUS_NOT_CHECKED;

    if (!is_bus_level_error(transfer_err) || ops == NULL || ops->recover == NULL) {
        return transfer_err;
    }

    i2c_line_result_t lines = I2C_LINES_IDLE;
    const esp_err_t recover_err = ops->recover(ops->context, &lines);
    if (recover_err == ESP_ERR_INVALID_STATE) {
        *out = NRF_TOPOLOGY_BUS_BACKOFF;
        return transfer_err;
    }
    if (recover_err != ESP_OK) {
        *out = NRF_TOPOLOGY_BUS_STUCK;
        return ESP_ERR_TIMEOUT;
    }
    if (lines == I2C_LINES_IDLE) {
        *out = NRF_TOPOLOGY_BUS_LINES_IDLE;
        return transfer_err;
    }

    /* A line was stuck and is free again: the TCA9548A may have latched a
     * partial control write -- close it and verify by readback. */
    if (ops->mux_close == NULL || ops->mux_close(ops->context) != ESP_OK) {
        *out = NRF_TOPOLOGY_BUS_RECOVERED_MUX_FAULT;
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = NRF_TOPOLOGY_BUS_RECOVERED_MUX_CLOSED;
    return transfer_err;
}
