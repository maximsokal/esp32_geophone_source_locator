#pragma once

#include <stdbool.h>

#include "esp_err.h"


typedef struct
{
    bool initialized;
    bool running;
    bool setup_mode;
    bool radio_state_uncertain;
    bool transition_active;
    bool network_events_suppressed;

} wifi_manager_internal_state_t;


void wifi_manager_internal_get_state(
    wifi_manager_internal_state_t *state
);

bool wifi_manager_internal_initialized(void);

/* Reserve the long-running Wi-Fi mode transition without holding the spinlock
 * across ESP-IDF Wi-Fi calls or lifecycle callbacks. Raw network callbacks are
 * non-authoritative until a final target/stopped state is committed. */
esp_err_t wifi_manager_internal_begin_transition(void);

void wifi_manager_internal_end_transition(void);

void wifi_manager_internal_set_initialized(
    bool initialized
);

/* Intermediate state commit inside an active radio transaction. This does not
 * reopen raw network-event authority. */
void wifi_manager_internal_commit_state(
    bool running,
    bool setup_mode,
    bool radio_state_uncertain
);

/* Final state commit: publishes the final manager snapshot and atomically
 * reopens raw network events before mode-completion reconciliation runs. */
void wifi_manager_internal_commit_final_state(
    bool running,
    bool setup_mode,
    bool radio_state_uncertain
);

bool wifi_manager_internal_setup_mode(void);

void wifi_manager_internal_set_radio_state_uncertain(
    bool uncertain
);

bool wifi_manager_internal_radio_state_uncertain(void);

esp_err_t wifi_manager_internal_start_setup_mode(void);
