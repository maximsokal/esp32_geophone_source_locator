#pragma once

#include "esp_err.h"

#include "keypad.h"


esp_err_t ui_init(void);

void ui_shutdown(void);


void ui_handle_keypad_event(
    keypad_key_t key,
    void *user_context
);
