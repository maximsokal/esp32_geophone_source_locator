#include "power_output.h"

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG =
    "POWER_CONTROL";


#define OFF_PULSE_MS  200


esp_err_t power_output_init(void)
{
    /*
     * MAX-246: load LOW into the output register BEFORE the pin becomes an
     * output. Configuring first would drive whatever the register still
     * holds; a stale 1 would momentarily assert OFF and cut the battery.
     */
    esp_err_t err =
        gpio_set_level(
            BOARD_POWER_OFF_GPIO,
            0
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set OFF GPIO LOW: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    const gpio_config_t off_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_POWER_OFF_GPIO,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    err =
        gpio_config(
            &off_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure OFF GPIO: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    return ESP_OK;
}


void power_output_pulse_off(void)
{
    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        1
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            OFF_PULSE_MS
        )
    );

    gpio_set_level(
        BOARD_POWER_OFF_GPIO,
        0
    );
}
