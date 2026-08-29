#pragma once

#include "esp_err.h"

#include "keypad.h"


/*
 * Start LVGL immediately after the physical display is initialized
 * and show the boot/status screen used during service bring-up.
 */
esp_err_t ui_boot_init(void);


void ui_boot_set_stage(
    const char *stage
);


void ui_boot_show_error(
    const char *message
);


/*
 * Switch from the boot screen to the interactive application UI.
 */
esp_err_t ui_init(void);


void ui_shutdown(void);


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
);
