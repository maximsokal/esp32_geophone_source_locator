#pragma once

#include "led_indicator.h"

typedef struct {
    bool is_active_level_high;
    int32_t gpio_num;
} led_indicator_gpio_config_t;

esp_err_t led_indicator_new_gpio_device(
    const led_indicator_config_t *config,
    const led_indicator_gpio_config_t *gpio,
    led_indicator_handle_t *handle);
