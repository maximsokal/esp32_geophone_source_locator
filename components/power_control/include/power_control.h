#pragma once

#include <stdbool.h>

#include "esp_err.h"


typedef void (*power_control_shutdown_notification_hook_t)(
    void *user_context
);


esp_err_t power_control_init(void);


/*
 * GPIO21 is initialized during early bring-up, but shutdown remains disarmed
 * until an explicit safe arm boundary is reached. Normal startup arms only
 * after the interactive runtime is ready; a terminal boot-error screen may
 * use the same arm primitive before waiting for a user-controlled shutdown.
 * The arm step confirms a released button; if the button is still held, the
 * official Button PRESS_UP callback completes arming after release, so the
 * pre-existing hold cannot itself latch shutdown.
 */
esp_err_t power_control_arm_shutdown_button(void);


/* Optional non-queue notification for UI/diagnostics. The shutdown latch is
 * set before this hook is called, so the hook can never be authoritative. */
esp_err_t power_control_set_shutdown_notification_hook(
    power_control_shutdown_notification_hook_t hook,
    void *user_context
);


void power_control_wait_for_shutdown_request(void);


bool power_control_is_shutdown_requested(void);


void power_control_shutdown(void);
