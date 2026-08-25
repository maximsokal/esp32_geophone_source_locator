#include "sd_card_internal.h"

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"


static const char *TAG =
    "SD_CARD";


esp_err_t sd_card_internal_prepare_shared_spi(void)
{
    const gpio_config_t tft_cs_config = {
        .pin_bit_mask =
            1ULL <<
            BOARD_TFT_CS_GPIO,

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_DISABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    esp_err_t err =
        gpio_config(
            &tft_cs_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure TFT CS: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    err =
        gpio_set_level(
            BOARD_TFT_CS_GPIO,
            1
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set TFT CS HIGH: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "TFT CS forced HIGH"
    );

    return ESP_OK;
}
