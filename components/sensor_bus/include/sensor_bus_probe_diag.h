#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensor_bus_probe_diag_policy.h"

typedef esp_err_t (*sensor_bus_probe_transfer_fn)(
    void *context,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len);

/* Run one existing driver call and preserve its returned error unchanged. */
esp_err_t sensor_bus_probe_diag_transaction(
    sensor_bus_probe_phase_t phase,
    uint8_t address,
    int channel,
    const uint8_t *tx,
    size_t tx_len,
    uint8_t *rx,
    size_t rx_len,
    bool expected_valid,
    uint8_t expected,
    sensor_bus_probe_transfer_fn transfer,
    void *context);

void sensor_bus_probe_diag_begin_pass(uint32_t pass_index);
void sensor_bus_probe_diag_end_pass(void);
void sensor_bus_probe_diag_log_bus_idle(void);
