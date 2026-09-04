#pragma once

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
