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

/**
 * @brief Initialize the ADC input used by the analog keypad.
 */
esp_err_t keypad_init(void);

/**
 * @brief Read the currently pressed key.
 *
 * This function returns the current stable analog-key state.
 */
esp_err_t keypad_read_key(keypad_key_t *key);

/**
 * @brief Return a human-readable key name.
 */
const char *keypad_key_name(keypad_key_t key);

/**
 * @brief Start a temporary event monitor.
 *
 * Unlike the calibration monitor, this prints only one line when
 * a key is pressed and one line when it is released.
 */
esp_err_t keypad_start_event_monitor(void);

/**
 * @brief Stop the temporary event monitor.
 */
void keypad_stop_event_monitor(void);