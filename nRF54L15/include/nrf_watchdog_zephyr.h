#ifndef NRF_WATCHDOG_ZEPHYR_H
#define NRF_WATCHDOG_ZEPHYR_H

#include "nrf_watchdog_health.h"

/*
 * MAX-244: WDT31 (devicetree alias watchdog0) resets the SoC when the main
 * loop stops feeding it for NRF_WATCHDOG_TIMEOUT_MS. The main loop feeds
 * only while nrf_watchdog_health reports every participant alive. After a
 * reset the node boots UNASSIGNED at the discovery address, which the
 * ESP32 topology reconciliation already re-assigns.
 *
 * Start once, after the runtimes it monitors are started. Returns a
 * negative errno when the watchdog could not be armed; the node then runs
 * unprotected (the caller reports it) rather than refusing to boot.
 */
int nrf_watchdog_zephyr_start(void);

/* Called by a monitored context on every loop iteration. */
void nrf_watchdog_zephyr_checkin(nrf_watchdog_participant_t participant);

/* Called from the main loop about once per second: probes the system
 * workqueue and feeds the hardware watchdog if everything is alive. */
void nrf_watchdog_zephyr_service(void);

/* Reset cause bits (hwinfo RESET_*) captured at start; 0 if unknown. */
uint32_t nrf_watchdog_zephyr_boot_reset_cause(void);

#endif
