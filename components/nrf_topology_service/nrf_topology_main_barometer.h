#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_status.h"
#include "main_barometer.h"

/*
 * MAIN's own BMP280 is polled from the nrf_topology task, not from the INA3221
 * task. Calibration reads the remote sensors and this barometer in the same
 * window, so one task serializes both and nothing can queue a poll behind a
 * topology bus session or the other way round.
 */
typedef struct {
    main_barometer_t barometer;
    bool published;
    device_main_status_t last_status;
} nrf_topology_main_barometer_t;

void nrf_topology_main_barometer_init(nrf_topology_main_barometer_t *state);
/* Poll when due, log a failure (rate limited) and publish when the reading changed. */
void nrf_topology_main_barometer_step(
    nrf_topology_main_barometer_t *state,
    uint64_t now_ms);
/* Longest the task may sleep without missing the next poll; at most max_ms. */
uint32_t nrf_topology_main_barometer_wait_ms(
    const nrf_topology_main_barometer_t *state,
    uint64_t now_ms,
    uint32_t max_ms);
void nrf_topology_main_barometer_close(nrf_topology_main_barometer_t *state);
