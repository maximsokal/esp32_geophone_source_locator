#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t power_control_arm_shutdown_button(void);
void power_control_wait_for_shutdown_request(void);
void power_control_shutdown(void);
