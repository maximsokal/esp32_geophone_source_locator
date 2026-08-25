#include "keypad.h"

#include <stddef.h>

#include <stdbool.h>

#include "esp_log.h"

#include "keypad_internal.h"


static const char *TAG =
    "KEYPAD";


static bool s_initialized =
    false;


esp_err_t keypad_init(void)
{
    if (s_initialized) {
        ESP_LOGW(
            TAG,
            "Keypad already initialized"
        );

        return ESP_OK;
    }

    esp_err_t err =
        keypad_adc_init();

    if (err != ESP_OK) {
        return err;
    }

    s_initialized =
        true;

    return ESP_OK;
}


esp_err_t keypad_read_key(
    keypad_key_t *key
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw =
        0;

    esp_err_t err =
        keypad_adc_read_averaged_raw(
            &raw
        );

    if (err != ESP_OK) {
        return err;
    }

    *key =
        keypad_decode_raw(
            raw
        );

    return ESP_OK;
}


const char *keypad_key_name(
    keypad_key_t key
)
{
    switch (key) {
        case KEYPAD_KEY_LEFT:
            return "LEFT";

        case KEYPAD_KEY_UP:
            return "UP";

        case KEYPAD_KEY_DOWN:
            return "DOWN";

        case KEYPAD_KEY_RIGHT:
            return "RIGHT";

        case KEYPAD_KEY_OK:
            return "OK";

        case KEYPAD_KEY_NONE:
        default:
            return "NONE";
    }
}


esp_err_t keypad_start_event_monitor(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return
        keypad_event_monitor_start(
            callback,
            user_context
        );
}


void keypad_stop_event_monitor(void)
{
    keypad_event_monitor_stop();
}
