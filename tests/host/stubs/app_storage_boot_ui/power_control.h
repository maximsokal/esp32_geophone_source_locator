#pragma once

#include "esp_err.h"

typedef void (*power_control_shutdown_notification_hook_t)(void *user_context);

esp_err_t power_control_arm_shutdown_button(void);
esp_err_t power_control_set_shutdown_notification_hook(
    power_control_shutdown_notification_hook_t hook,
    void *user_context
);
void power_control_wait_for_shutdown_request(void);
void power_control_shutdown(void);
