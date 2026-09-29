#include <assert.h>
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
static uint16_t s_events[4];
static uint32_t s_event_arg0[4];
static uint32_t s_event_arg1[4];
static unsigned s_event_count;

int64_t k_uptime_get(void)
{
    return s_now_ms;
}

int k_work_submit(struct k_work *work)
{
    assert(work != NULL);
    ++s_work_submits;
    return 1;
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
    return 0;
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
    (void)channel_id;
    return 0;
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
    assert(nrf_watchdog_zephyr_start() == 0);
    assert(s_reset_cause_reads == 1U && s_reset_cause_clears == 1U);
    assert(s_watchdog_installs == 1U && s_watchdog_setups == 1U);

    s_now_ms = NRF_WATCHDOG_MAX_SILENCE_MS + 1U;
    nrf_watchdog_zephyr_service();
    nrf_watchdog_zephyr_service();
    assert(s_event_count == 1U);
    assert(s_events[0] == NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE);
    assert(s_event_arg0[0] == NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    assert(s_event_arg1[0] == NRF_WATCHDOG_MAX_SILENCE_MS);

    ++s_now_ms;
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SENSOR_WORKER);
    nrf_watchdog_zephyr_service();
    nrf_watchdog_zephyr_service();
    assert(s_event_count == 2U);
    assert(s_events[1] == NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE);
    assert(s_event_arg0[1] == NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE);
    assert(s_event_arg1[1] == NRF_WATCHDOG_MAX_SILENCE_MS);
    assert(s_work_submits == 4U);
    return 0;
}
