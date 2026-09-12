#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct button_driver_t button_driver_t;

struct button_driver_t {
    bool enable_power_save;
    uint8_t (*get_key_level)(button_driver_t *button_driver);
    esp_err_t (*enter_power_save)(button_driver_t *button_driver);
    esp_err_t (*exit_power_save)(button_driver_t *button_driver);
    int32_t (*get_gpio_num)(button_driver_t *button_driver);
    esp_err_t (*del)(button_driver_t *button_driver);
};
