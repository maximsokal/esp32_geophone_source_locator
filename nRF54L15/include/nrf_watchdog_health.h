#ifndef NRF_WATCHDOG_HEALTH_H
#define NRF_WATCHDOG_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * MAX-244: the hardware watchdog is fed from the main loop only while every
 * monitored execution context has proven progress recently. A hung main
 * loop stops feeding by itself; a hung worker makes the main loop stop.
 * CS procedures are asynchronous (callbacks + delayable timeout) and never
 * block a monitored context, so no participant needs a CS-length allowance.
 */
typedef enum {
    NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER = 0,
    NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE,
    NRF_WATCHDOG_PARTICIPANT_COUNT
} nrf_watchdog_participant_t;

#define NRF_WATCHDOG_PARTICIPANT_NONE NRF_WATCHDOG_PARTICIPANT_COUNT

/* A participant silent for longer than this stops the feed. */
#define NRF_WATCHDOG_MAX_SILENCE_MS 5000U
/* Hardware timeout; a stopped feed resets the node within this. */
#define NRF_WATCHDOG_TIMEOUT_MS 8000U

typedef struct {
    uint64_t last_checkin_ms[NRF_WATCHDOG_PARTICIPANT_COUNT];
    uint32_t max_silence_ms;
    bool initialized;
} nrf_watchdog_health_t;

void nrf_watchdog_health_init(
    nrf_watchdog_health_t *health,
    uint64_t now_ms,
    uint32_t max_silence_ms);

void nrf_watchdog_health_checkin(
    nrf_watchdog_health_t *health,
    nrf_watchdog_participant_t participant,
    uint64_t now_ms);

/* True when every participant checked in within max_silence_ms. Otherwise
 * `stale` (may be NULL) names the first silent participant. */
bool nrf_watchdog_health_should_feed(
    const nrf_watchdog_health_t *health,
    uint64_t now_ms,
    nrf_watchdog_participant_t *stale);

const char *nrf_watchdog_participant_name(nrf_watchdog_participant_t participant);

#endif
