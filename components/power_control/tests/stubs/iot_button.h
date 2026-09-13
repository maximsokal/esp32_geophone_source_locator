#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef void *button_handle_t;
typedef void (*button_cb_t)(void *button_handle, void *user_data);

typedef enum {
    BUTTON_PRESS_DOWN = 0,
    BUTTON_PRESS_UP,
    BUTTON_PRESS_REPEAT,
    BUTTON_PRESS_REPEAT_DONE,
    BUTTON_SINGLE_CLICK,
    BUTTON_DOUBLE_CLICK,
    BUTTON_MULTIPLE_CLICK,
    BUTTON_LONG_PRESS_START,
    BUTTON_LONG_PRESS_HOLD,
    BUTTON_LONG_PRESS_UP,
    BUTTON_PRESS_END,
    BUTTON_EVENT_MAX,
    BUTTON_NONE_PRESS,
} button_event_t;

typedef struct {
    uint16_t long_press_time;
    uint16_t short_press_time;
} button_config_t;

esp_err_t iot_button_new_gpio_device(
    const button_config_t *button_config,
    const void *gpio_config,
    button_handle_t *ret_button
);
esp_err_t iot_button_register_cb(
    button_handle_t button,
    button_event_t event,
    void *event_args,
    button_cb_t callback,
    void *user_data
);
button_event_t iot_button_get_event(button_handle_t button);
uint8_t iot_button_get_key_level(button_handle_t button);
esp_err_t iot_button_delete(button_handle_t button);
