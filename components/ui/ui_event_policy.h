#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Navigation is an immediate interaction, not a command queue. If LVGL was
 * prevented from running for longer than four frames, replaying old button
 * presses makes the menu jump several items after the stall ends. Expire only
 * keypad navigation; status/power/network events keep their normal ordering.
 * The screen runs at 4 FPS (ui_perf_policy.h) and handles input once per frame,
 * so a press waits up to 250 ms by design, plus up to two postponed ticks while the SD card is busy.
 */
#define UI_KEYPAD_EVENT_MAX_AGE_US 1000000LL

static inline bool ui_event_keypad_is_stale(
    int64_t queued_us,
    int64_t now_us
)
{
    if (now_us < queued_us) {
        return false;
    }

    return
        now_us - queued_us >
        UI_KEYPAD_EVENT_MAX_AGE_US;
}


/* Queued Wi-Fi APP_EVENTs are observations too. Accept their notices only in
 * the currently authoritative mode, after final network state publication. */
static inline bool ui_event_setup_client_notice_allowed(
    bool wifi_running,
    bool setup_mode,
    bool radio_state_uncertain,
    bool network_events_suppressed
)
{
    return
        wifi_running &&
        setup_mode &&
        !radio_state_uncertain &&
        !network_events_suppressed;
}


static inline bool ui_event_sta_notice_allowed(
    bool wifi_running,
    bool setup_mode,
    bool radio_state_uncertain,
    bool network_events_suppressed
)
{
    return
        wifi_running &&
        !setup_mode &&
        !radio_state_uncertain &&
        !network_events_suppressed;
}
