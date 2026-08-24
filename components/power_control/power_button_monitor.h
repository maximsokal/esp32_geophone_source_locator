#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t power_button_monitor_init(void);


void power_button_monitor_wait_for_shutdown_request(void);


bool power_button_monitor_is_shutdown_requested(void);


void power_button_monitor_stop(void);
