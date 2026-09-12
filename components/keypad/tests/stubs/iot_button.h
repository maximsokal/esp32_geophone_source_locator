#pragma once

#include <stddef.h>
#include <stdint.h>

#include "button_types.h"

typedef void (*button_cb_t)(void *button_handle, void *usr_data);

typedef enum {
    BUTTON_PRESS_DOWN = 0,
} button_event_t;

typedef struct {
    uint16_t long_press_time;
    uint16_t short_press_time;
} button_config_t;

typedef union {
    struct {
        uint16_t press_time;
    } long_press;
} button_event_args_t;

esp_err_t iot_button_create(
    const button_config_t *config,
    const button_driver_t *driver,
    button_handle_t *ret_button
);

esp_err_t iot_button_delete(button_handle_t btn_handle);

esp_err_t iot_button_register_cb(
    button_handle_t btn_handle,
    button_event_t event,
    button_event_args_t *event_args,
    button_cb_t cb,
    void *usr_data
);
