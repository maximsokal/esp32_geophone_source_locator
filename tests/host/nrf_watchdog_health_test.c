#include <assert.h>
#include <string.h>

#include "nrf_watchdog_health.h"

/* MAX-244: the feed decision behind the XIAO hardware watchdog. */

static void test_fresh_start_feeds(void)
{
    nrf_watchdog_health_t h;
    nrf_watchdog_health_init(&h, 1000U, NRF_WATCHDOG_MAX_SILENCE_MS);
    nrf_watchdog_participant_t stale = NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER;
    assert(nrf_watchdog_health_should_feed(&h, 1000U, &stale));
    assert(stale == NRF_WATCHDOG_PARTICIPANT_NONE);
    /* Exactly at the limit is still healthy. */
    assert(nrf_watchdog_health_should_feed(&h, 1000U + NRF_WATCHDOG_MAX_SILENCE_MS, NULL));
}

static void test_each_silent_participant_stops_the_feed(void)
{
    for (int silent = 0; silent < (int)NRF_WATCHDOG_PARTICIPANT_COUNT; ++silent) {
        nrf_watchdog_health_t h;
        nrf_watchdog_health_init(&h, 0U, 5000U);
        uint64_t t = 0U;
        for (int step = 0; step < 10; ++step) {
            t += 1000U;
            for (int p = 0; p < (int)NRF_WATCHDOG_PARTICIPANT_COUNT; ++p) {
                if (p != silent) {
                    nrf_watchdog_health_checkin(&h, (nrf_watchdog_participant_t)p, t);
                }
            }
            nrf_watchdog_participant_t stale;
            const bool feed = nrf_watchdog_health_should_feed(&h, t, &stale);
            if (t <= 5000U) {
                assert(feed);
            } else {
                assert(!feed);
                assert(stale == (nrf_watchdog_participant_t)silent);
            }
        }
    }
}

static void test_all_alive_feeds_forever(void)
{
    nrf_watchdog_health_t h;
    nrf_watchdog_health_init(&h, 0U, 5000U);
    for (uint64_t t = 1000U; t < 600000U; t += 1000U) {
        nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER, t);
        nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE, t);
        assert(nrf_watchdog_health_should_feed(&h, t, NULL));
    }
}

static void test_recovery_and_races(void)
{
    nrf_watchdog_health_t h;
    nrf_watchdog_health_init(&h, 0U, 5000U);
    assert(!nrf_watchdog_health_should_feed(&h, 6000U, NULL));
    nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER, 6000U);
    nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE, 6000U);
    assert(nrf_watchdog_health_should_feed(&h, 6000U, NULL));
    /* A check-in newer than the feeder's clock reading is not "stale". */
    nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER, 7000U);
    assert(nrf_watchdog_health_should_feed(&h, 6500U, NULL));
}

static void test_guards(void)
{
    nrf_watchdog_health_t h;
    memset(&h, 0, sizeof(h));
    /* Uninitialized state never feeds (fail towards reset, not away). */
    assert(!nrf_watchdog_health_should_feed(&h, 0U, NULL));
    assert(!nrf_watchdog_health_should_feed(NULL, 0U, NULL));
    nrf_watchdog_health_checkin(NULL, NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER, 1U);
    nrf_watchdog_health_init(&h, 0U, 5000U);
    nrf_watchdog_health_checkin(&h, NRF_WATCHDOG_PARTICIPANT_COUNT, 99999U); /* ignored */
    nrf_watchdog_health_init(NULL, 0U, 1U);
    assert(strcmp(nrf_watchdog_participant_name(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER), "sensor_worker") == 0);
    assert(strcmp(nrf_watchdog_participant_name(NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE), "system_workqueue") == 0);
    assert(strcmp(nrf_watchdog_participant_name(NRF_WATCHDOG_PARTICIPANT_NONE), "none") == 0);
    /* Hardware timeout leaves margin over the software silence limit. */
    assert(NRF_WATCHDOG_TIMEOUT_MS > NRF_WATCHDOG_MAX_SILENCE_MS);
}

int main(void)
{
    test_fresh_start_feeds();
    test_each_silent_participant_stops_the_feed();
    test_all_alive_feeds_forever();
    test_recovery_and_races();
    test_guards();
    return 0;
}
