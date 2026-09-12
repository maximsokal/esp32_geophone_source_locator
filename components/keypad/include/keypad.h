#pragma once

#include "esp_err.h"

typedef enum
{
    KEYPAD_KEY_NONE = 0,
    KEYPAD_KEY_LEFT,
    KEYPAD_KEY_UP,
    KEYPAD_KEY_DOWN,
    KEYPAD_KEY_RIGHT,
    KEYPAD_KEY_OK
} keypad_key_t;

/* Called once for every debounced key press. */
typedef void (*keypad_event_callback_t)(
    keypad_key_t key,
    void *user_context
);

esp_err_t keypad_init(void);

/* Compatibility/readout API for diagnostics; event delivery uses Button and
 * APP_LOOP below. */
esp_err_t keypad_read_key(keypad_key_t *key);

const char *keypad_key_name(keypad_key_t key);

esp_err_t keypad_start_event_monitor(
    keypad_event_callback_t callback,
    void *user_context
);

void keypad_stop_event_monitor(void);
