#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


esp_err_t wifi_manager_init(void);

esp_err_t wifi_manager_start(void);

esp_err_t wifi_manager_stop(void);

bool wifi_manager_is_running(void);

bool wifi_manager_is_setup_mode(void);


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

bool wifi_manager_mdns_is_ready(void);
