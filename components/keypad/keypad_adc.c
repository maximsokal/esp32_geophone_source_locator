#include "keypad_internal.h"

#include <stddef.h>

#include "adc_service.h"
#include "board_config.h"

#include "esp_log.h"


static const char *TAG =
    "KEYPAD";


#define KEYPAD_SAMPLE_COUNT    16


static adc_channel_t s_adc_channel;


esp_err_t keypad_adc_init(void)
{
    esp_err_t err =
        adc_service_init();

    if (err != ESP_OK) {
        return err;
    }

    err =
        adc_service_configure_gpio(
            BOARD_KEYPAD_GPIO,
            ADC_ATTEN_DB_12,
            ADC_BITWIDTH_DEFAULT,
            &s_adc_channel
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure keypad ADC: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "KS0466 initialized: GPIO%d, ADC1 channel %d",
        BOARD_KEYPAD_GPIO,
        (int)s_adc_channel
    );

    return ESP_OK;
}


esp_err_t keypad_adc_read_averaged_raw(
    int *raw_value
)
{
    return
        adc_service_read_average_raw(
            s_adc_channel,
            KEYPAD_SAMPLE_COUNT,
            raw_value
        );
}
