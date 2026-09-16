#include "wifi_manager_event_policy.h"


bool wifi_manager_event_epoch_matches(
    bool started_enabled,
    uint32_t started_generation,
    bool current_enabled,
    uint32_t current_generation
)
{
    return
        started_enabled &&
        current_enabled &&
        started_generation == current_generation;
}


bool wifi_manager_sta_ip_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_associated,
    bool live_ip_valid,
    bool current_enabled,
    uint32_t current_generation
)
{
    return
        live_associated &&
        live_ip_valid &&
        wifi_manager_event_epoch_matches(
            started_enabled,
            started_generation,
            current_enabled,
            current_generation
        );
}


bool wifi_manager_sta_disconnect_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_associated,
    bool current_enabled,
    uint32_t current_generation
)
{
    return
        !live_associated &&
        wifi_manager_event_epoch_matches(
            started_enabled,
            started_generation,
            current_enabled,
            current_generation
        );
}


bool wifi_manager_setup_client_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool live_client_present,
    bool current_enabled,
    uint32_t current_generation
)
{
    return
        live_client_present &&
        wifi_manager_event_epoch_matches(
            started_enabled,
            started_generation,
            current_enabled,
            current_generation
        );
}


bool wifi_manager_setup_disconnect_observation_accept(
    bool started_enabled,
    uint32_t started_generation,
    bool any_live_client_present,
    bool current_enabled,
    uint32_t current_generation
)
{
    return
        !any_live_client_present &&
        wifi_manager_event_epoch_matches(
            started_enabled,
            started_generation,
            current_enabled,
            current_generation
        );
}
