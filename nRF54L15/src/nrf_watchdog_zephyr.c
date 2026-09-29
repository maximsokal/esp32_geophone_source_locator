#include "nrf_watchdog_zephyr.h"

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"

static const struct device *const s_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

static struct k_spinlock s_lock;
static nrf_watchdog_health_t s_health;
static struct k_work s_workqueue_probe;
static int s_channel = -1;
static bool s_armed;
static bool s_stale_reported;
static nrf_watchdog_participant_t s_stale_reported_participant;
static uint32_t s_boot_reset_cause;
static bool s_reset_cause_captured;

static uint64_t uptime_ms(void)
{
    return (uint64_t)k_uptime_get();
}

static void workqueue_probe_handler(struct k_work *work)
{
    (void)work;
    nrf_watchdog_zephyr_checkin(NRF_WATCHDOG_PARTICIPANT_SYSTEM_WORKQUEUE);
}

static void capture_reset_cause(void)
{
    uint32_t cause = 0U;

    if (s_reset_cause_captured) {
        return;
    }
    if (hwinfo_get_reset_cause(&cause) != 0) {
        return;
    }
    s_boot_reset_cause = cause;
    s_reset_cause_captured = true;
    (void)hwinfo_clear_reset_cause();
}

int nrf_watchdog_zephyr_start(void)
{
    const struct wdt_timeout_cfg timeout = {
        .window = {
            .min = 0U,
            .max = NRF_WATCHDOG_TIMEOUT_MS,
        },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };
    k_spinlock_key_t key;
    int result;

    capture_reset_cause();
    if (s_reset_cause_captured) {
        if ((s_boot_reset_cause & RESET_WATCHDOG) != 0U) {
            printk("nrf_watchdog: previous run was reset by the WATCHDOG (cause 0x%08x)\n", s_boot_reset_cause);
        } else {
            printk("nrf_watchdog: reset cause 0x%08x\n", s_boot_reset_cause);
        }
    }

    key = k_spin_lock(&s_lock);
    nrf_watchdog_health_init(&s_health, uptime_ms(), NRF_WATCHDOG_MAX_SILENCE_MS);
    k_spin_unlock(&s_lock, key);
    k_work_init(&s_workqueue_probe, workqueue_probe_handler);

    if (!device_is_ready(s_wdt)) {
        return -ENODEV;
    }
    result = wdt_install_timeout(s_wdt, &timeout);
    if (result < 0) {
        return result;
    }
    s_channel = result;
    /* Keep counting while the CPU sleeps (a sleep deadlock is a hang too);
     * pause only while a debugger halts the core. */
    result = wdt_setup(s_wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (result < 0) {
        return result;
    }
    s_armed = true;
    return 0;
}

void nrf_watchdog_zephyr_checkin(nrf_watchdog_participant_t participant)
{
    k_spinlock_key_t key = k_spin_lock(&s_lock);

    nrf_watchdog_health_checkin(&s_health, participant, uptime_ms());
    k_spin_unlock(&s_lock, key);
}

void nrf_watchdog_zephyr_service(void)
{
    nrf_watchdog_participant_t stale = NRF_WATCHDOG_PARTICIPANT_NONE;
    k_spinlock_key_t key;
    bool feed;

    /* The probe checks in when the system workqueue runs it; a busy or
     * blocked workqueue leaves the previous submission pending. */
    (void)k_work_submit(&s_workqueue_probe);

    key = k_spin_lock(&s_lock);
    feed = nrf_watchdog_health_should_feed(&s_health, uptime_ms(), &stale);
    k_spin_unlock(&s_lock, key);

    if (!feed) {
        if (!s_stale_reported || stale != s_stale_reported_participant) {
            printk("nrf_watchdog: %s silent > %u ms, feed stopped; reset in <= %u ms\n",
                   nrf_watchdog_participant_name(stale),
                   (unsigned)NRF_WATCHDOG_MAX_SILENCE_MS,
                   (unsigned)NRF_WATCHDOG_TIMEOUT_MS);
            s_stale_reported = true;
            s_stale_reported_participant = stale;
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_WATCHDOG_PARTICIPANT_STALE,
                (uint32_t)stale,
                NRF_WATCHDOG_MAX_SILENCE_MS);
        }
        return;
    }
    s_stale_reported = false;
    s_stale_reported_participant = NRF_WATCHDOG_PARTICIPANT_NONE;
    if (s_armed) {
        (void)wdt_feed(s_wdt, s_channel);
    }
}

uint32_t nrf_watchdog_zephyr_boot_reset_cause(void)
{
    capture_reset_cause();
    return s_boot_reset_cause;
}
