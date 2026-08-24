#pragma once

#include <stdbool.h>

#include "esp_err.h"


esp_err_t power_control_init(void);


void power_control_wait_for_shutdown_request(void);


bool power_control_is_shutdown_requested(void);


void power_control_shutdown(void);