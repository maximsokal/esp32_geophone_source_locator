#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool wifi_manager_is_setup_mode(void);
esp_err_t wifi_manager_save_sta_credentials(const char *, const char *);
esp_err_t wifi_manager_connect_saved(void);
