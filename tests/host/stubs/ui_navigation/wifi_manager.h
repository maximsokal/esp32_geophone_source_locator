#pragma once

#include <stdbool.h>

/* Host navigation tests do not exercise Wi-Fi notices. Keep production
 * ui_navigation.c linkable while defaulting queued Wi-Fi events to denied. */
static inline bool wifi_manager_is_running(void)
{
    return false;
}

static inline bool wifi_manager_is_setup_mode(void)
{
    return false;
}

static inline bool wifi_manager_is_radio_state_uncertain(void)
{
    return false;
}

static inline bool wifi_manager_network_events_suppressed(void)
{
    return true;
}

static inline bool wifi_manager_sta_has_ip(void)
{
    return false;
}
