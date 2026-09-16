#pragma once

#include <stdbool.h>
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
 * Authoritative shutdown closes new mutating Web command admission. Keep this
 * as a pure policy so host tests can prove the boundary without an HTTP stack.
 */
static inline bool web_lifecycle_command_ingress_allowed(bool shutdown_requested)
{
    return !shutdown_requested;
}


/*
 * Small JSON/form bodies are bounded requests, not bulk transfers. A socket
 * timeout therefore terminates the current request instead of retrying it.
 */
static inline bool web_lifecycle_small_body_timeout_retry_allowed(void)
{
    return false;
}


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


/*
 * Apply the radio-safety override to a state derived from a network event.
 * The caller must evaluate this at the lifecycle enqueue boundary.
 */
web_lifecycle_state_t web_lifecycle_effective_network_state(
    web_lifecycle_state_t event_state,
    bool radio_state_uncertain
);
