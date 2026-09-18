#pragma once

#include "esp_err.h"


/*
 * Starts the emergency-stop background task (MAX-118). The task itself
 * decides, tick by tick, whether a recording is active and only evaluates
 * the trigger while one is -- there is nothing else to start/stop this
 * against, so callers invoke this once at boot (alongside
 * nrf_topology_service_start()) and never again.
 */
esp_err_t nrf_emergency_stop_monitor_start(void);
