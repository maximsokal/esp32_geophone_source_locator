#pragma once

#include <stdint.h>

#include "esp_err.h"


/*
 * Start the low-rate INA3221 power monitor.
 *
 * Missing/faulty INA3221 hardware is handled by the worker and does not make
 * application startup fail. The worker keeps retrying and publishes invalid
 * power data until communication recovers.
 */
esp_err_t ina3221_monitor_start(void);


/*
 * Stop the monitor task and release its I2C bus/device handles.
 */
void ina3221_monitor_stop(void);


/*
 * Switch the monitor to a 20 ms sample period and log every sample
 * (bypassing the normal 5-sample throttle) for the next duration_ms, then
 * revert to the normal 1 s / throttled behaviour automatically.
 *
 * The steady 1 Hz rate cannot resolve a brownout-causing current transient
 * that develops over tens of milliseconds; this gives a fine-grained voltage
 * trace bracketing one specific risky call (e.g. esp_wifi_start()) without
 * paying the 20 ms rate for the whole run. Safe to call even if the monitor
 * task has not started yet or hardware is absent -- the request is just
 * picked up whenever the worker next checks it.
 */
void ina3221_monitor_request_fast_sampling(
    uint32_t duration_ms
);
