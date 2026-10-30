#include "ui_power_badge.h"
#include "ui_i18n.h"
#include "ui_utf8_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>


static int clamp_percent(float percent)
{
    if (!isfinite(percent) || percent <= 0.0f) {
        return 0;
    }
    if (percent >= 100.0f) {
        return 100;
    }
    return (int)(percent + 0.5f);
}


static ui_power_icon_t battery_icon(int percent)
{
    if (percent >= 85) {
        return UI_POWER_ICON_BATTERY_FULL;
    }
    if (percent >= 60) {
        return UI_POWER_ICON_BATTERY_3;
    }
    if (percent >= 35) {
        return UI_POWER_ICON_BATTERY_2;
    }
    if (percent >= 10) {
        return UI_POWER_ICON_BATTERY_1;
    }
    return UI_POWER_ICON_BATTERY_EMPTY;
}


static void set(ui_power_badge_t *badge, ui_power_icon_t icon, ui_power_severity_t severity, const char *text)
{
    badge->icon = icon;
    badge->severity = severity;
    ui_utf8_format(badge->text, sizeof(badge->text), "%s", text);
}


void ui_power_banner_from_status(
    const device_power_status_t *power,
    ui_power_banner_t *banner)
{
    if (banner == NULL) {
        return;
    }
    memset(banner, 0, sizeof(*banner));
    banner->severity = UI_POWER_SEVERITY_ALERT;
    if (power == NULL) {
        return;
    }
    if (power->low_battery == DEVICE_LOW_BATTERY_SHUTDOWN) {
        banner->visible = true;
        ui_utf8_format(banner->text, sizeof(banner->text), "%s", ui_tr(UI_I18N_KEY_POWER_BANNER_STOP));
    } else if (power->low_battery == DEVICE_LOW_BATTERY_GRACE) {
        banner->visible = true;
        ui_utf8_format(
            banner->text,
            sizeof(banner->text),
            ui_tr(UI_I18N_KEY_POWER_BANNER_GRACE),
            (unsigned)(power->shutdown_in_s % 1000U));
    }
}


void ui_power_badge_from_status(
    const device_power_status_t *power,
    ui_power_badge_t *badge)
{
    if (badge == NULL) {
        return;
    }
    if (power == NULL) {
        set(badge, UI_POWER_ICON_NONE, UI_POWER_SEVERITY_MUTED, ui_tr(UI_I18N_KEY_HEADER_POWER_EMPTY));
        return;
    }

    char percent_text[UI_POWER_BADGE_TEXT_MAX];
    const bool percent_valid = power->battery_percent_valid &&
        isfinite(power->battery_percent_estimate);
    const int percent = percent_valid ? clamp_percent(power->battery_percent_estimate) : 0;
    ui_utf8_format(percent_text, sizeof(percent_text), ui_tr(UI_I18N_KEY_POWER_PERCENT), percent);

    /* The low-battery reaction outranks every other state. */
    if (power->low_battery == DEVICE_LOW_BATTERY_SHUTDOWN) {
        set(badge, UI_POWER_ICON_BATTERY_EMPTY, UI_POWER_SEVERITY_ALERT, ui_tr(UI_I18N_KEY_POWER_OFF));
        return;
    }
    if (power->low_battery == DEVICE_LOW_BATTERY_GRACE) {
        char text[UI_POWER_BADGE_TEXT_MAX];
        ui_utf8_format(text, sizeof(text), ui_tr(UI_I18N_KEY_POWER_GRACE), (unsigned)(power->shutdown_in_s % 1000U));
        set(badge, UI_POWER_ICON_BATTERY_EMPTY, UI_POWER_SEVERITY_ALERT, text);
        return;
    }

    switch (power->state) {
        case DEVICE_POWER_STATE_USB_ONLY_NO_BATTERY:
            set(badge, UI_POWER_ICON_USB, UI_POWER_SEVERITY_NORMAL, ui_tr(UI_I18N_KEY_POWER_USB));
            return;
        case DEVICE_POWER_STATE_USB_CHARGING:
            set(badge, UI_POWER_ICON_CHARGE, UI_POWER_SEVERITY_GOOD,
                percent_valid ? percent_text : ui_tr(UI_I18N_KEY_POWER_CHARGING));
            return;
        case DEVICE_POWER_STATE_USB_BATTERY_READY:
            if (power->battery_full) {
                set(badge, UI_POWER_ICON_BATTERY_FULL, UI_POWER_SEVERITY_GOOD, ui_tr(UI_I18N_KEY_POWER_FULL));
            } else {
                set(badge, UI_POWER_ICON_USB, UI_POWER_SEVERITY_NORMAL,
                    percent_valid ? percent_text : ui_tr(UI_I18N_KEY_POWER_USB));
            }
            return;
        case DEVICE_POWER_STATE_BATTERY:
            set(badge, battery_icon(percent), UI_POWER_SEVERITY_NORMAL,
                percent_valid ? percent_text : ui_tr(UI_I18N_KEY_POWER_BATTERY));
            return;
        case DEVICE_POWER_STATE_BATTERY_LOW:
            set(badge, UI_POWER_ICON_BATTERY_1, UI_POWER_SEVERITY_WARNING, ui_tr(UI_I18N_KEY_POWER_BADGE_LOW));
            return;
        case DEVICE_POWER_STATE_BATTERY_CRITICAL:
            set(badge, UI_POWER_ICON_BATTERY_EMPTY, UI_POWER_SEVERITY_ALERT, ui_tr(UI_I18N_KEY_POWER_CRITICAL));
            return;
        case DEVICE_POWER_STATE_UNKNOWN:
        default:
            set(badge, UI_POWER_ICON_NONE, UI_POWER_SEVERITY_MUTED, ui_tr(UI_I18N_KEY_HEADER_POWER_EMPTY));
            return;
    }
}
