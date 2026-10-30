#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_status.h"

/*
 * MAX-182/183: the power badge in the TFT header, shown on every screen.
 * Pure mapping from device_status power fields to an icon + short text +
 * severity; ui_shell turns the icon into an LVGL symbol and the severity
 * into a colour. The header slot fits about 6 characters.
 */

typedef enum
{
    UI_POWER_ICON_NONE = 0,
    UI_POWER_ICON_USB,
    UI_POWER_ICON_CHARGE,
    UI_POWER_ICON_BATTERY_FULL,
    UI_POWER_ICON_BATTERY_3,
    UI_POWER_ICON_BATTERY_2,
    UI_POWER_ICON_BATTERY_1,
    UI_POWER_ICON_BATTERY_EMPTY,

} ui_power_icon_t;

typedef enum
{
    UI_POWER_SEVERITY_MUTED = 0,
    UI_POWER_SEVERITY_NORMAL,
    UI_POWER_SEVERITY_GOOD,
    UI_POWER_SEVERITY_WARNING,
    UI_POWER_SEVERITY_ALERT,

} ui_power_severity_t;

#define UI_POWER_BADGE_TEXT_MAX 12U

typedef struct
{
    ui_power_icon_t icon;
    ui_power_severity_t severity;
    char text[UI_POWER_BADGE_TEXT_MAX];

} ui_power_badge_t;

void ui_power_badge_from_status(
    const device_power_status_t *power,
    ui_power_badge_t *badge);

/*
 * Critical battery: a full-width bar over the footer of every screen. The header badge only has
 * room for "OFF" or "USB 8s", and the per-screen notice is read by a few screens only, so a
 * measurement or ranging screen would otherwise say nothing while the device stops.
 *   GRACE     -> "CRITICAL - USB 8s OR STOP"
 *   SHUTDOWN  -> "CRITICAL BATTERY - STOPPING"
 * Nothing is shown in any other state.
 */
/* UTF-8: Ukrainian text takes two bytes per letter, three for the dash. */
#define UI_POWER_BANNER_TEXT_MAX 96U

typedef struct
{
    bool visible;
    ui_power_severity_t severity;
    char text[UI_POWER_BANNER_TEXT_MAX];

} ui_power_banner_t;

void ui_power_banner_from_status(
    const device_power_status_t *power,
    ui_power_banner_t *banner);
