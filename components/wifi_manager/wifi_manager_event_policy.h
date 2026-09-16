#pragma once

#include <stdbool.h>
#include <stdint.h>


/*
 * Event payloads are only wakeups. A handler may commit an observation only
 * when the admission epoch it started in is still the current enabled epoch.
 */
bool wifi_manager_event_epoch_matches(
    bool started_enabled,
    uint32_t started_generation,
    bool current_enabled,
    uint32_t current_generation
);


bool wifi_manager_sta_ip_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_associated,
    bool live_ip_valid,
    bool current_enabled,
    uint32_t current_generation
);


bool wifi_manager_sta_disconnect_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_associated,
    bool current_enabled,
    uint32_t current_generation
);


bool wifi_manager_setup_client_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_client_present,
    bool current_enabled,
    uint32_t current_generation
);


bool wifi_manager_setup_disconnect_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool any_live_client_present,
    bool current_enabled,
    uint32_t current_generation
);
