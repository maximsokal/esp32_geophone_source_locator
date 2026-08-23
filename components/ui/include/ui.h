#pragma once

#include "esp_err.h"

#include "keypad.h"

/*
 * Initialize UI state and draw the home screen.
 */
esp_err_t ui_init(void);

/*
 * Keypad event callback.
 */
void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
);