#include <assert.h>
#include <math.h>
#include <string.h>

#include "ui_power_badge.h"
#include "ui_i18n.h"

/* MAX-182/183: TFT header power badge. */

static device_power_status_t power_with(device_power_state_t state, float percent)
{
    device_power_status_t p;
    memset(&p, 0, sizeof(p));
    p.state = state;
    p.battery_percent_valid = true;
    p.battery_percent_estimate = percent;
    return p;
}

static ui_power_badge_t badge_of(const device_power_status_t *p)
{
    ui_power_badge_t b;
    ui_power_badge_from_status(p, &b);
    return b;
}

static void test_battery_levels(void)
{
    device_power_status_t p = power_with(DEVICE_POWER_STATE_BATTERY, 87.4f);
    ui_power_badge_t b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_BATTERY_FULL && strcmp(b.text, "87%") == 0);
    assert(b.severity == UI_POWER_SEVERITY_NORMAL);
    p.battery_percent_estimate = 62.0f;
    assert(badge_of(&p).icon == UI_POWER_ICON_BATTERY_3);
    p.battery_percent_estimate = 40.0f;
    assert(badge_of(&p).icon == UI_POWER_ICON_BATTERY_2);
    p.battery_percent_estimate = 12.0f;
    assert(badge_of(&p).icon == UI_POWER_ICON_BATTERY_1);
    p.battery_percent_estimate = 3.0f;
    assert(badge_of(&p).icon == UI_POWER_ICON_BATTERY_EMPTY);
    p.battery_percent_estimate = 140.0f;
    assert(strcmp(badge_of(&p).text, "100%") == 0);
    p.battery_percent_estimate = NAN;
    assert(strcmp(badge_of(&p).text, "BAT") == 0);
    p.battery_percent_valid = false;
    assert(strcmp(badge_of(&p).text, "BAT") == 0);
}

static void test_usb_states(void)
{
    device_power_status_t p = power_with(DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY, 0.0f);
    ui_power_badge_t b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_USB && strcmp(b.text, "USB") == 0);

    p = power_with(DEVICE_POWER_STATE_USB_CHARGING, 55.0f);
    b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_CHARGE && strcmp(b.text, "55%") == 0 && b.severity == UI_POWER_SEVERITY_GOOD);
    p.battery_percent_valid = false;
    assert(strcmp(badge_of(&p).text, "CHG") == 0);

    p = power_with(DEVICE_POWER_STATE_USB_BATTERY_READY, 98.0f);
    p.battery_full = true;
    b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_BATTERY_FULL && strcmp(b.text, "FULL") == 0);
    p.battery_full = false;
    b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_USB && strcmp(b.text, "98%") == 0);
    p.battery_percent_valid = false;
    assert(strcmp(badge_of(&p).text, "USB") == 0);
}

static void test_low_battery_alerts_outrank_everything(void)
{
    device_power_status_t p = power_with(DEVICE_POWER_STATE_BATTERY_LOW, 8.0f);
    ui_power_badge_t b = badge_of(&p);
    assert(strcmp(b.text, "LOW") == 0 && b.severity == UI_POWER_SEVERITY_WARNING);

    p = power_with(DEVICE_POWER_STATE_BATTERY_CRITICAL, 1.0f);
    b = badge_of(&p);
    assert(strcmp(b.text, "CRIT") == 0 && b.severity == UI_POWER_SEVERITY_ALERT);

    /* Countdown to the shutdown: "connect USB" with the seconds left. */
    p.low_battery = DEVICE_LOW_BATTERY_GRACE;
    p.shutdown_in_s = 8U;
    b = badge_of(&p);
    assert(strcmp(b.text, "USB 8s") == 0 && b.severity == UI_POWER_SEVERITY_ALERT);
    assert(b.icon == UI_POWER_ICON_BATTERY_EMPTY);
    /* Even if the state momentarily reads BATTERY again. */
    p.state = DEVICE_POWER_STATE_BATTERY;
    assert(strcmp(badge_of(&p).text, "USB 8s") == 0);
    /* Text always fits the slot. */
    p.shutdown_in_s = 120U;
    assert(strlen(badge_of(&p).text) < UI_POWER_BADGE_TEXT_MAX);

    p.low_battery = DEVICE_LOW_BATTERY_SHUTDOWN;
    b = badge_of(&p);
    assert(strcmp(b.text, "OFF") == 0 && b.severity == UI_POWER_SEVERITY_ALERT);
}

static void test_unknown_and_null(void)
{
    device_power_status_t p = power_with(DEVICE_POWER_STATE_UNKNOWN, 50.0f);
    ui_power_badge_t b = badge_of(&p);
    assert(b.icon == UI_POWER_ICON_NONE && strcmp(b.text, "--") == 0 && b.severity == UI_POWER_SEVERITY_MUTED);
    ui_power_badge_from_status(NULL, &b);
    assert(strcmp(b.text, "--") == 0);
    ui_power_badge_from_status(&p, NULL);
}

int main(void)
{
    ui_i18n_set_locale(UI_LOCALE_EN);
    test_battery_levels();
    test_usb_states();
    test_low_battery_alerts_outrank_everything();
    test_unknown_and_null();

    ui_i18n_set_locale(UI_LOCALE_UK);
    device_power_status_t p = power_with(DEVICE_POWER_STATE_USB_CHARGING, 0.0f);
    p.battery_percent_valid = false;
    assert(strcmp(badge_of(&p).text, "ЗАРЯД") == 0);
    p = power_with(DEVICE_POWER_STATE_USB_BATTERY_READY, 0.0f);
    p.battery_full = true;
    assert(strcmp(badge_of(&p).text, "ПОВНА") == 0);
    p = power_with(DEVICE_POWER_STATE_BATTERY_LOW, 8.0f);
    assert(strcmp(badge_of(&p).text, "НИЗЬК.") == 0);
    p = power_with(DEVICE_POWER_STATE_BATTERY_CRITICAL, 1.0f);
    assert(strcmp(badge_of(&p).text, "КРИТ.") == 0);
    p.low_battery = DEVICE_LOW_BATTERY_GRACE;
    p.shutdown_in_s = 8U;
    assert(strcmp(badge_of(&p).text, "USB 8с") == 0);
    return 0;
}
