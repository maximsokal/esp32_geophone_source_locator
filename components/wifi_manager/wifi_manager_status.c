#include "wifi_manager.h"

#include "wifi_manager_internal.h"
#include "wifi_manager_runtime.h"
#include "wifi_manager_sta_events.h"


bool wifi_manager_is_transition_active(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.transition_active;
}


bool wifi_manager_network_events_suppressed(void)
{
    wifi_manager_internal_state_t state;
    wifi_manager_internal_get_state(&state);
    return state.network_events_suppressed;
}


bool wifi_manager_sta_has_ip(void)
{
    char text[16];
    return
        wifi_manager_runtime_sta_ip_string(
            text,
            sizeof(text)
        ) == ESP_OK;
}


esp_err_t wifi_manager_get_reconnect_status(
    wifi_manager_reconnect_status_t *status
)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_sta_events_get_reconnect_status(
        &status->last_reason,
        &status->retry_count,
        &status->next_delay_ms
    );
    return ESP_OK;
}


bool wifi_manager_setup_client_connected(void)
{
    return
        wifi_manager_runtime_setup_client_connected();
}


bool wifi_manager_setup_client_known(void)
{
    return
        wifi_manager_runtime_setup_client_known();
}


esp_err_t wifi_manager_setup_client_mac_string(
    char *text,
    size_t text_size
)
{
    return
        wifi_manager_runtime_setup_client_mac_string(
            text,
            text_size
        );
}


esp_err_t wifi_manager_sta_ip_string(
    char *text,
    size_t text_size
)
{
    return
        wifi_manager_runtime_sta_ip_string(
            text,
            text_size
        );
}
