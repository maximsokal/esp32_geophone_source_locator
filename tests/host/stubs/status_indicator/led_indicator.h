#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *led_indicator_handle_t;
typedef struct {
    const void **blink_lists;
    uint16_t blink_list_num;
} led_indicator_config_t;

esp_err_t led_indicator_set_on_off(led_indicator_handle_t handle, bool on);
esp_err_t led_indicator_delete(led_indicator_handle_t handle);
