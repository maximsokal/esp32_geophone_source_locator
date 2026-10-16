#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>

#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_watchdog_zephyr.h"

struct device s_test_watchdog_device = {.ready = 1};

static int64_t s_now_ms;
static unsigned s_reset_cause_reads;
static unsigned s_reset_cause_clears;
static unsigned s_watchdog_installs;
static unsigned s_watchdog_setups;
static unsigned s_work_submits;
static unsigned s_coalesced_submits;
static bool s_work_pending;
static struct k_work *s_pending_work;
static int s_watchdog_install_result;
static int s_installed_channel = 7;
static int s_feed_result;
static int s_last_feed_channel = -1;
static unsigned s_watchdog_feeds;
static uint16_t s_events[8];
static uint32_t s_event_arg0[8];
static uint32_t s_event_arg1[8];
static unsigned s_event_count;

int64_t k_uptime_get(void)
{
    return s_now_ms;
}

int k_work_submit(struct k_work *work)
{
    assert(work != NULL);
    ++s_work_submits;
    if (s_work_pending) {
        ++s_coalesced_submits;
        return 0;
    }
    s_work_pending = true;
    s_pending_work = work;
    return 1;
}

static void run_pending_work(void)
{
    struct k_work *work;

    assert(s_work_pending);
    assert(s_pending_work != NULL);
    work = s_pending_work;
    s_work_pending = false;
    s_pending_work = NULL;
    work->handler(work);
}

int hwinfo_get_reset_cause(uint32_t *cause)
{
    assert(cause != NULL);
    ++s_reset_cause_reads;
    *cause = RESET_WATCHDOG;
    return 0;
}

int hwinfo_clear_reset_cause(void)
{
    ++s_reset_cause_clears;
    return 0;
}

int wdt_install_timeout(const struct device *device, const struct wdt_timeout_cfg *config)
{
    assert(device == &s_test_watchdog_device);
    assert(config->window.max == NRF_WATCHDOG_TIMEOUT_MS);
    ++s_watchdog_installs;
    return s_watchdog_install_result == 0
        ? s_installed_channel
        : s_watchdog_install_result;
}

int wdt_setup(const struct device *device, uint8_t options)
{
    assert(device == &s_test_watchdog_device);
    assert(options == WDT_OPT_PAUSE_HALTED_BY_DBG);
    ++s_watchdog_setups;
    return 0;
}

int wdt_feed(const struct device *device, int channel_id)
{
    assert(device == &s_test_watchdog_device);
    assert(channel_id == s_installed_channel);
    s_last_feed_channel = channel_id;
    ++s_watchdog_feeds;
    return s_feed_result;
}

int printk(const char *format, ...)
{
    (void)format;
    return 0;
}

void nrf_log_runtime_zephyr_append_event(uint16_t event_id, uint32_t arg0, uint32_t arg1)
{
    assert(s_event_count < sizeof(s_events) / sizeof(s_events[0]));
    s_events[s_event_count] = event_id;
    s_event_arg0[s_event_count] = arg0;
    s_event_arg1[s_event_count] = arg1;
    ++s_event_count;
}

int main(void)
{
    assert(nrf_watchdog_zephyr_boot_reset_cause() == RESET_WATCHDOG);
    assert(s_reset_cause_reads == 1U && s_reset_cause_clears == 1U);
    s_watchdog_install_result = -ENODEV;
    assert(nrf_watchdog_zephyr_start() == -ENODEV);
    assert(nrf_watchdog_zephyr_is_degraded());
    assert(s_event_count == 1U);
    assert(s_events[0] == NRF_LOG_EVENT_WATCHDOG_DEGRADED);
    assert(s_event_arg0[0] == NRF_WATCHDOG_ARM_STAGE_INSTALL);
    assert((int32_t)s_event_arg1[0] == -ENODEV);

    s_watchdog_install_result = 0;
    assert(nrf_watchdog_zephyr_start() == 0);
    assert(!nrf_watchdog_zephyr_is_degraded());
    assert(s_reset_cause_reads == 1U && s_reset_cause_clears == 1U);
    assert(s_watchdog_installs == 2U && s_watchdog_setups == 1U);
    assert(s_installed_channel == 7);

    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE);
    nrf_watchdog_zephyr_service();
    assert(s_watchdog_feeds == 1U && s_last_feed_channel == s_installed_channel);

    /* A blocked system workqueue leaves the probe pending despite coalesced submissions. */
    for (s_now_ms = 1000U; s_now_ms <= 6000U; s_now_ms += 1000U) {
        nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
        nrf_watchdog_zephyr_service();
        assert(s_watchdog_feeds == 1U + (s_now_ms <= NRF_WATCHDOG_MAX_SILENCE_MS
            ? (unsigned)(s_now_ms / 1000U)
            : 5U));
    }
    s_now_ms = 6000U;
    assert(s_coalesced_submits == 6U);
    assert(s_work_pending && s_pending_work != NULL);
    assert(s_event_count == 2U);
    assert(s_events[1] == NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE);
    assert(s_event_arg0[1] == NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE);

    run_pending_work();
    nrf_watchdog_zephyr_set_participant_active(
        NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS,
        true);
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    nrf_watchdog_zephyr_service();
    assert(s_watchdog_feeds == 7U);
    run_pending_work();

    /* Repeated active=true calls must not renew the CS check-in grace. */
    for (s_now_ms = 7000U; s_now_ms <= 12000U; s_now_ms += 1000U) {
        nrf_watchdog_zephyr_set_participant_active(
            NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS,
            true);
        nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
        nrf_watchdog_zephyr_service();
        run_pending_work();
    }
    assert(s_watchdog_feeds == 12U);
    assert(s_event_count == 3U);
    assert(s_events[2] == NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE);
    assert(s_event_arg0[2] == NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);

    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    nrf_watchdog_zephyr_service();
    assert(s_watchdog_feeds == 13U);
    run_pending_work();

    s_feed_result = -EIO;
    s_now_ms = 12001U;
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE);
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_BT_CS_PROGRESS);
    nrf_watchdog_zephyr_service();
    assert(s_watchdog_feeds == 14U);
    assert(nrf_watchdog_zephyr_feed_error_total() == 1U);
    assert(nrf_watchdog_zephyr_is_degraded());
    assert(s_event_count == 4U);
    assert(s_events[3] == NRF_LOG_EVENT_WATCHDOG_FEED_FAILED);
    assert((int32_t)s_event_arg1[3] == -EIO);
    assert(s_work_submits == 16U);
    return 0;
}
