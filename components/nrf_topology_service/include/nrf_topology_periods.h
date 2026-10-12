#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef CONFIG_NRF_LOG_POLL_PERIOD_MS
#define CONFIG_NRF_LOG_POLL_PERIOD_MS 1000U
#endif

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
