#pragma once

#include <stdbool.h>
#include <stddef.h>
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


/* Zero-based INA3221 channel indices (physical CH1..CH3). */
#define INA3221_MONITOR_RAIL_BATTERY 0U
#define INA3221_MONITOR_RAIL_ESP32_5V 1U
#define INA3221_MONITOR_RAIL_AD7606_5V 2U

typedef struct
{
    /* At least one read attempt has been published for this channel. */
    bool available;
    /* The last read of this channel succeeded (false after an INA3221 fault). */
    bool valid;
    float bus_voltage_v;
    float current_a;
    /* esp_timer time of the last published read (0 when never). */
    int64_t sampled_at_us;

    /* MAX-245 software over-current state: 0 none, 1 warning, 2 critical
     * (ina3221_overcurrent_level_t). A confirmed critical stays latched
     * until ina3221_monitor_clear_overcurrent(). */
    int overcurrent_level;
    bool overcurrent_latched;
    float overcurrent_latched_current_a;

} ina3221_monitor_rail_t;

/*
 * MAX-242: the latest published reading of one rail with its timestamp, so
 * a consumer (the AD7606 power interlock) can tell a fresh reading from a
 * stale one left behind by a stopped monitor.
 */
esp_err_t ina3221_monitor_get_rail(
    size_t channel_index,
    ina3221_monitor_rail_t *rail_out
);

/*
 * MAX-245: clear a latched over-current of one channel (the published
 * state immediately, the worker's tracker at its next sample). If the
 * over-current persists it is confirmed and latched again within two
 * samples.
 */
esp_err_t ina3221_monitor_clear_overcurrent(
    size_t channel_index
);

