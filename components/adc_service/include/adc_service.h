#pragma once

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"


esp_err_t adc_service_init(void);

/* ADC1 is the service-owned conversion unit. This metadata accessor is only
 * for peripherals that need to create a matching calibration descriptor; all
 * raw conversions must go through adc_service_read_*(). */
adc_unit_t adc_service_unit(void);

esp_err_t adc_service_configure_gpio(
    gpio_num_t gpio,
    adc_atten_t attenuation,
    adc_bitwidth_t bitwidth,
    adc_channel_t *channel
);

esp_err_t adc_service_read_raw(
    adc_channel_t channel,
    int *raw_value
);

/* Every service read discards the first conversion while holding the ADC1
 * service mutex. All application ADC consumers use this API; no borrowed
 * ADC1 handle is exposed. */
esp_err_t adc_service_read_average_raw(
    adc_channel_t channel,
    size_t sample_count,
    int *raw_value
);
