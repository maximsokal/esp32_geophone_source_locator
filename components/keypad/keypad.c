#include "keypad.h"

#include <stdbool.h>

#include "app_events.h"
#include "esp_log.h"


static const char *TAG = "KEYPAD";
static bool s_initialized = false;


esp_err_t adc_service_init(void);


/* The adapter owns the vendor Button objects. Keep these declarations local
 * so the public keypad header remains a small application-facing API. */
esp_err_t keypad_button_adapter_init(void);
esp_err_t keypad_button_adapter_start(
    keypad_event_callback_t callback,
    void *user_context
);
void keypad_button_adapter_stop(void);
esp_err_t keypad_button_adapter_start_boot(
    keypad_event_callback_t callback,
    void *user_context
);
void keypad_button_adapter_stop_boot(void);
esp_err_t keypad_button_adapter_read_key(keypad_key_t *key);


esp_err_t keypad_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Keypad already initialized");
        return ESP_OK;
    }

    /* Button objects begin polling during early bring-up, but navigation must
     * not enter APP_EVENT until the interactive UI subscriber is ready. */
    app_events_set_keypad_delivery_enabled(false);

    esp_err_t err = adc_service_init();
    if (err != ESP_OK) {
        return err;
    }

    err = keypad_button_adapter_init();
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}


esp_err_t keypad_read_key(keypad_key_t *key)
{
    return keypad_button_adapter_read_key(key);
}


const char *keypad_key_name(keypad_key_t key)
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

    const esp_err_t err =
        keypad_button_adapter_start(callback, user_context);

    if (err == ESP_OK) {
        /* Startup calls this only after ui_init() has installed its APP_EVENT
         * subscriber. No press sampled before this point can be replayed. */
        app_events_set_keypad_delivery_enabled(true);
    }

    return err;
}


void keypad_stop_event_monitor(void)
{
    /* Close the event gate before deleting Button objects so an in-flight
     * callback cannot enqueue navigation during shutdown. */
    app_events_set_keypad_delivery_enabled(false);
    keypad_button_adapter_stop();
    s_initialized = false;
}


esp_err_t keypad_start_boot_event_monitor(
    keypad_event_callback_t callback,
    void *user_context
)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return keypad_button_adapter_start_boot(callback, user_context);
}


void keypad_stop_boot_event_monitor(void)
{
    keypad_button_adapter_stop_boot();
}
