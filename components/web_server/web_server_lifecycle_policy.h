#pragma once

#include <stddef.h>


typedef enum {
    WEB_LIFECYCLE_STATE_STA_CONNECTED = 0,
    WEB_LIFECYCLE_STATE_SETUP_AP,
    WEB_LIFECYCLE_STATE_STA_DISCONNECTED,
    WEB_LIFECYCLE_STATE_QUIESCED,
    WEB_LIFECYCLE_STATE_STOPPED,
} web_lifecycle_state_t;


typedef enum {
    WEB_LIFECYCLE_ACTION_MDNS_START = 0,
    WEB_LIFECYCLE_ACTION_MDNS_STOP,
    WEB_LIFECYCLE_ACTION_HTTP_START,
    WEB_LIFECYCLE_ACTION_HTTP_STOP,
    WEB_LIFECYCLE_ACTION_HTTPS_START,
    WEB_LIFECYCLE_ACTION_HTTPS_STOP,
    WEB_LIFECYCLE_ACTION_EXIT,
} web_lifecycle_action_t;


#define WEB_LIFECYCLE_ACTIONS_MAX 4U


/*
 * Return the ordered, idempotent actions for the desired network state.
 * Returning the full plan makes a one-slot queue safe: only the final desired
 * state matters, and the worker always applies all required side effects.
 */
size_t web_lifecycle_state_actions(
    web_lifecycle_state_t state,
    web_lifecycle_action_t *actions,
    size_t action_capacity
);
