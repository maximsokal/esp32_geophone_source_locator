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

} wifi_manager_internal_state_t;


void wifi_manager_internal_get_state(
    wifi_manager_internal_state_t *state
);

bool wifi_manager_internal_initialized(void);

/* Reserve the long-running Wi-Fi mode transition without holding the spinlock
 * across ESP-IDF Wi-Fi calls or lifecycle callbacks. */
esp_err_t wifi_manager_internal_begin_transition(void);

void wifi_manager_internal_end_transition(void);

void wifi_manager_internal_set_initialized(
    bool initialized
);

/* Commit externally visible radio state atomically. setup_mode is forced false
 * whenever running is false so readers never observe an impossible snapshot. */
void wifi_manager_internal_commit_state(
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
