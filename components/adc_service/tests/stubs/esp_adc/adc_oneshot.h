#pragma once

#include "esp_err.h"

typedef int adc_atten_t;
typedef int adc_bitwidth_t;
typedef int adc_channel_t;
typedef int adc_unit_t;
typedef void *adc_oneshot_unit_handle_t;

esp_err_t adc_oneshot_read(
    adc_oneshot_unit_handle_t handle,
    adc_channel_t channel,
    int *raw_value
);
