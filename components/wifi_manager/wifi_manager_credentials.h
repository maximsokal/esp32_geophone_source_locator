#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"


esp_err_t wifi_manager_credentials_has_saved(
    bool *has_credentials
);

esp_err_t wifi_manager_credentials_save(
    const char *ssid,
    const char *password
);

esp_err_t wifi_manager_credentials_clear(void);

esp_err_t wifi_manager_credentials_get_ssid(
    char *ssid,
    size_t ssid_size
);
