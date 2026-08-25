#pragma once

#include "esp_err.h"

#include "keypad.h"


esp_err_t keypad_adc_init(void);


esp_err_t keypad_adc_read_averaged_raw(
    int *raw_value
);


keypad_key_t keypad_decode_raw(
    int raw
);


esp_err_t keypad_event_monitor_start(
    keypad_event_callback_t callback,
    void *user_context
);


void keypad_event_monitor_stop(void);
