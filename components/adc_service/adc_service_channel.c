#include "adc_service.h"

#include "adc_service_internal.h"

#include "esp_log.h"


static const char *TAG =
    "ADC_SERVICE";


esp_err_t adc_service_configure_gpio(
    gpio_num_t gpio,
    adc_atten_t attenuation,
    adc_bitwidth_t bitwidth,
    adc_channel_t *channel
)
{
    if (!g_adc_service_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (channel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_unit_t unit;
    adc_channel_t resolved_channel;

    esp_err_t err =
        adc_oneshot_io_to_channel(
            gpio,
            &unit,
            &resolved_channel
        );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "GPIO%d is not an ADC input: %s",
            (int)gpio,
            esp_err_to_name(err)
        );

        return err;
    }

    if (unit != ADC_UNIT_1) {
        ESP_LOGE(
            TAG,
            "GPIO%d is not on ADC1",
            (int)gpio
        );

        return ESP_ERR_NOT_SUPPORTED;
    }

    const adc_oneshot_chan_cfg_t config = {
        .bitwidth = bitwidth,
        .atten = attenuation,
    };

    err =
        adc_oneshot_config_channel(
            g_adc_service_handle,
            resolved_channel,
            &config
        );

    if (err != ESP_OK) {
        return err;
    }

    *channel =
        resolved_channel;

    ESP_LOGI(
        TAG,
        "Configured GPIO%d as ADC1 channel %d",
        (int)gpio,
        (int)resolved_channel
    );

    return ESP_OK;
}
