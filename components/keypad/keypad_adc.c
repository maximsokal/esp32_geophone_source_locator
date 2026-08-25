#include "keypad_internal.h"

#include <stddef.h>

#include "board_config.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"


static const char *TAG =
    "KEYPAD";


#define KEYPAD_SAMPLE_COUNT  16


static adc_oneshot_unit_handle_t s_adc_handle =
    NULL;


static adc_unit_t s_adc_unit;


static adc_channel_t s_adc_channel;


esp_err_t keypad_adc_init(void)
{
    esp_err_t err =
        adc_oneshot_io_to_channel(
            BOARD_KEYPAD_GPIO,
            &s_adc_unit,
            &s_adc_channel
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d is not a valid ADC GPIO: %s",
            BOARD_KEYPAD_GPIO,
            esp_err_to_name(err)
        );

        return err;
    }

    const adc_oneshot_unit_init_cfg_t
        unit_config = {
            .unit_id =
                s_adc_unit,

            .ulp_mode =
                ADC_ULP_MODE_DISABLE,
        };

    err =
        adc_oneshot_new_unit(
            &unit_config,
            &s_adc_handle
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create ADC unit: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    const adc_oneshot_chan_cfg_t
        channel_config = {
            .bitwidth =
                ADC_BITWIDTH_DEFAULT,

            .atten =
                ADC_ATTEN_DB_12,
        };

    err =
        adc_oneshot_config_channel(
            s_adc_handle,
            s_adc_channel,
            &channel_config
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure ADC channel: %s",
            esp_err_to_name(err)
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "KS0466 initialized: GPIO%d, ADC%d channel %d",
        BOARD_KEYPAD_GPIO,
        (int)s_adc_unit + 1,
        (int)s_adc_channel
    );

    return ESP_OK;
}


esp_err_t keypad_adc_read_averaged_raw(
    int *raw_value
)
{
    if (raw_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_sum =
        0;

    for (
        int i = 0;
        i < KEYPAD_SAMPLE_COUNT;
        ++i
    ) {
        int raw =
            0;

        esp_err_t err =
            adc_oneshot_read(
                s_adc_handle,
                s_adc_channel,
                &raw
            );

        if (err != ESP_OK) {
            return err;
        }

        raw_sum +=
            raw;
    }

    *raw_value =
        raw_sum /
        KEYPAD_SAMPLE_COUNT;

    return ESP_OK;
}
