#include "power_button_input.h"

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"


static const char *TAG =
    "POWER_BUTTON";


esp_err_t power_button_input_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_POWER_BUTTON_GPIO,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &button_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure button GPIO: %s",
            esp_err_to_name(err)
        );
    }

    return err;
}


bool power_button_input_is_high(void)
{
    return
        gpio_get_level(
            BOARD_POWER_BUTTON_GPIO
        ) == 1;
}
