#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


esp_err_t wifi_manager_init(void);

esp_err_t wifi_manager_start(void);

esp_err_t wifi_manager_stop(void);

bool wifi_manager_is_running(void);

bool wifi_manager_is_setup_mode(void);

bool wifi_manager_is_radio_state_uncertain(void);


typedef esp_err_t (*wifi_manager_radio_stop_prepare_callback_t)(void *context);
typedef void (*wifi_manager_radio_stop_complete_callback_t)(
    void *context,
    esp_err_t radio_stop_result
);

typedef void (*wifi_manager_mode_transition_complete_callback_t)(
    void *context,
    bool running,
    bool setup_mode,
    esp_err_t transition_result,
    esp_err_t cleanup_result
);

/*
 * Register the synchronous radio-stop lifecycle hooks. The prepare callback
 * must return only after network services have stopped resources that must
 * not outlive the Wi-Fi mode transition. The complete callback runs after
 * esp_wifi_stop() returns, including when it returns an error, and receives
 * the normalized lifecycle result (`ESP_ERR_WIFI_NOT_STARTED` is ESP_OK).
 */
esp_err_t wifi_manager_set_radio_stop_lifecycle_callbacks(
    wifi_manager_radio_stop_prepare_callback_t prepare_callback,
    wifi_manager_radio_stop_complete_callback_t complete_callback,
    void *context
);

/* Compatibility wrapper for callers that only need the prepare phase. */
esp_err_t wifi_manager_set_radio_stop_prepare_callback(
    wifi_manager_radio_stop_prepare_callback_t callback,
    void *context
);

/*
 * Register the final-state notification for a complete mode transition.
 * The callback runs after the target mode has either committed or failed and
 * receives the manager's final running/setup state. A failed target start is
 * reported with a stopped/disconnected state. cleanup_result is ESP_OK when
 * the target cleanup succeeded (including WIFI_NOT_STARTED); a non-OK value
 * means that the radio state is uncertain and must not be treated as a clean
 * stopped state.
 */
esp_err_t wifi_manager_set_mode_transition_complete_callback(
    wifi_manager_mode_transition_complete_callback_t callback,
    void *context
);


esp_err_t wifi_manager_enter_setup_mode(void);

esp_err_t wifi_manager_connect_saved(void);

esp_err_t wifi_manager_save_sta_credentials(
    const char *ssid,
    const char *password
);

esp_err_t wifi_manager_forget_sta_credentials(void);

esp_err_t wifi_manager_get_saved_ssid(
    char *ssid,
    size_t ssid_size
);


bool wifi_manager_setup_client_connected(void);

bool wifi_manager_setup_client_known(void);

esp_err_t wifi_manager_setup_client_mac_string(
    char *text,
    size_t text_size
);

esp_err_t wifi_manager_sta_ip_string(
    char *text,
    size_t text_size
);


/* mDNS is initialized by the web lifecycle task after STA obtains an IP. */
esp_err_t wifi_manager_mdns_init(void);

void wifi_manager_mdns_stop(void);

bool wifi_manager_mdns_is_ready(void);
