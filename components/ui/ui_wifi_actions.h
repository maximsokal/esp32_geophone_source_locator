#pragma once

#include "esp_err.h"


esp_err_t ui_wifi_action_activate_setup(void);

esp_err_t ui_wifi_action_open_setup(void);

esp_err_t ui_wifi_action_use_quick_preset(void);

esp_err_t ui_wifi_action_connect_saved(void);

esp_err_t ui_wifi_action_forget(void);
