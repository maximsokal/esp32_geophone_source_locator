#include <assert.h>
#include <string.h>

#include "ina3221_low_battery.h"

/* MAX-183: low-battery warning, grace countdown, cancel and shutdown. */

#define GRACE_MS 10000U

static void test_warning_is_reported_once_and_clears(void)
{
    ina3221_low_battery_t p;
    ina3221_low_battery_init(&p, GRACE_MS);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY, 0U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_LOW, 1000U) == INA3221_LOW_BATTERY_ACTION_WARN);
    assert(p.phase == INA3221_LOW_BATTERY_WARNING);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_LOW, 2000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    /* Missing telemetry does not clear the warning. */
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_UNKNOWN, 3000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(p.phase == INA3221_LOW_BATTERY_WARNING);
    /* USB does. */
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_USB_CHARGING, 4000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(p.phase == INA3221_LOW_BATTERY_NORMAL);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_LOW, 5000U) == INA3221_LOW_BATTERY_ACTION_WARN);
}

static void test_critical_counts_down_then_requests_shutdown_once(void)
{
    ina3221_low_battery_t p;
    ina3221_low_battery_init(&p, GRACE_MS);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 100000U) == INA3221_LOW_BATTERY_ACTION_START_GRACE);
    assert(p.phase == INA3221_LOW_BATTERY_GRACE);
    assert(ina3221_low_battery_remaining_s(&p, 100000U) == 10U);
    assert(ina3221_low_battery_remaining_s(&p, 104500U) == 6U);

    /* Unknown telemetry or a voltage rebound during grace do not cancel. */
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_UNKNOWN, 103000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_LOW, 106000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY, 109999U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(p.phase == INA3221_LOW_BATTERY_GRACE);

    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 110000U) == INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN);
    assert(p.phase == INA3221_LOW_BATTERY_SHUTDOWN);
    assert(ina3221_low_battery_remaining_s(&p, 110000U) == 0U);

    /* Irreversible: USB after the request changes nothing; no repeat. */
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_USB_CHARGING, 111000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 200000U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(p.phase == INA3221_LOW_BATTERY_SHUTDOWN);
}

static void test_usb_during_grace_cancels(void)
{
    ina3221_low_battery_t p;
    ina3221_low_battery_init(&p, GRACE_MS);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 0U) == INA3221_LOW_BATTERY_ACTION_START_GRACE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_USB_CHARGING, 9000U) == INA3221_LOW_BATTERY_ACTION_CANCELLED);
    assert(p.phase == INA3221_LOW_BATTERY_NORMAL);
    /* A later critical starts a fresh, full countdown. */
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 50000U) == INA3221_LOW_BATTERY_ACTION_START_GRACE);
    assert(ina3221_low_battery_remaining_s(&p, 50000U) == 10U);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 59999U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 60000U) == INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN);
}

static void test_zero_grace_and_guards(void)
{
    ina3221_low_battery_t p;
    ina3221_low_battery_init(&p, 0U);
    assert(ina3221_low_battery_update(&p, INA3221_POWER_UX_BATTERY_CRITICAL, 5U) == INA3221_LOW_BATTERY_ACTION_REQUEST_SHUTDOWN);
    assert(ina3221_low_battery_update(NULL, INA3221_POWER_UX_BATTERY_CRITICAL, 5U) == INA3221_LOW_BATTERY_ACTION_NONE);
    assert(ina3221_low_battery_remaining_s(NULL, 0U) == 0U);
    ina3221_low_battery_init(NULL, 1U);
    assert(strcmp(ina3221_low_battery_phase_name(INA3221_LOW_BATTERY_GRACE), "grace") == 0);
    assert(strcmp(ina3221_low_battery_phase_name(INA3221_LOW_BATTERY_SHUTDOWN), "shutdown") == 0);
}

int main(void)
{
    test_warning_is_reported_once_and_clears();
    test_critical_counts_down_then_requests_shutdown_once();
    test_usb_during_grace_cancels();
    test_zero_grace_and_guards();
    return 0;
}
