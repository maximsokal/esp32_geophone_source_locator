#pragma once

#include "esp_err.h"


esp_err_t wifi_manager_sta_init(void);

esp_err_t wifi_manager_sta_start(void);

void wifi_manager_sta_reset_state(void);
