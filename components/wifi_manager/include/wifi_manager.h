#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t wifi_manager_init(void);

esp_err_t wifi_manager_start(void);

esp_err_t wifi_manager_stop(void);

bool wifi_manager_is_running(void);