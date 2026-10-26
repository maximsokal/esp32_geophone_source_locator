#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef CONFIG_NRF_LOG_POLL_PERIOD_MS
#define CONFIG_NRF_LOG_POLL_PERIOD_MS 1000U
#endif

/*
 * The nRF I2C target (TWIS) is driven by an interrupt: after the address phase it
 * holds SCL low until software has prepared the response. The controller's
 * default 2 ms stretch limit aborted reads of MAIN right after a log request
 * (logs/esp32 2026-10-26: "I2C transaction timeout detected" on 0x31). The
 * transfer's own software timeout still bounds a target that never answers.
 */
#define NRF_TOPOLOGY_TARGET_SCL_WAIT_US 25000U

#define NRF_TOPOLOGY_RECONCILE_PERIOD_MS 5000U
#define NRF_LOG_COLLECTOR_PERIOD_MS CONFIG_NRF_LOG_POLL_PERIOD_MS
#define NRF_SENSOR_REFRESH_PERIOD_MS 2000U
#define NRF_CS_PUMP_INTERVAL_MS 10U
#define NRF_TOPOLOGY_STALE_LIMIT_MS (5U * NRF_TOPOLOGY_RECONCILE_PERIOD_MS)
#define NRF_TOPOLOGY_PERIOD_TOLERANCE_MS 50U
#define NRF_CS_PUMP_TOLERANCE_MS 2U

static inline bool nrf_topology_period_elapsed(uint64_t elapsed_ms, uint32_t period_ms)
{
    return elapsed_ms >= period_ms;
}
