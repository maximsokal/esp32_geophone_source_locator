#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int32_t gpio_num;
    uint8_t active_level;
    bool enable_power_save;
    bool disable_pull;
} button_gpio_config_t;
