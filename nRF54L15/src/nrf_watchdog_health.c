#include "nrf_watchdog_health.h"

#include <stddef.h>
#include <string.h>

void nrf_watchdog_health_init(
    nrf_watchdog_health_t *health,
    uint64_t now_ms,
    uint32_t max_silence_ms)
{
    if (health == NULL) {
        return;
    }
    memset(health, 0, sizeof(*health));
    for (size_t i = 0; i < NRF_WATCHDOG_PARTICIPANT_COUNT; ++i) {
        health->last_checkin_ms[i] = now_ms;
    }
    health->active[NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER] = true;
    health->active[NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE] = true;
    health->max_silence_ms = max_silence_ms;
    health->initialized = true;
}

void nrf_watchdog_health_checkin(
    nrf_watchdog_health_t *health,
    nrf_watchdog_participant_t participant,
    uint64_t now_ms)
{
    if (health == NULL || !health->initialized ||
        (unsigned)participant >= (unsigned)NRF_WATCHDOG_PARTICIPANT_COUNT) {
        return;
    }
    health->last_checkin_ms[participant] = now_ms;
}

void nrf_watchdog_health_set_active(
    nrf_watchdog_health_t *health,
    nrf_watchdog_participant_t participant,
    bool active,
    uint64_t now_ms)
{
    if (health == NULL || !health->initialized ||
        (unsigned)participant >= (unsigned)NRF_WATCHDOG_PARTICIPANT_COUNT) {
        return;
    }
    health->active[participant] = active;
    if (active) {
        health->last_checkin_ms[participant] = now_ms;
    }
}

bool nrf_watchdog_health_should_feed(
    const nrf_watchdog_health_t *health,
    uint64_t now_ms,
    nrf_watchdog_participant_t *stale)
{
    if (stale != NULL) {
        *stale = NRF_WATCHDOG_PARTICIPANT_NONE;
    }
    if (health == NULL || !health->initialized) {
        return false;
    }
    for (size_t i = 0; i < NRF_WATCHDOG_PARTICIPANT_COUNT; ++i) {
        if (!health->active[i]) {
            continue;
        }
        const uint64_t last = health->last_checkin_ms[i];
        /* A check-in stamped after `now` (read race between contexts) is
         * fresh, not an unsigned wrap-around. */
        if (now_ms > last && now_ms - last > health->max_silence_ms) {
            if (stale != NULL) {
                *stale = (nrf_watchdog_participant_t)i;
            }
            return false;
        }
    }
    return true;
}

const char *nrf_watchdog_participant_name(nrf_watchdog_participant_t participant)
{
    switch (participant) {
    case NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER:
        return "sensor_worker";
    case NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE:
        return "system_workqueue";
    case NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS:
        return "bt_cs_progress";
    default:
        return "none";
    }
}
