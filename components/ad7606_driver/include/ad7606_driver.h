#pragma once

#include <stdint.h>

#include "esp_err.h"


#define AD7606_CHANNEL_COUNT 8


typedef struct
{
    int16_t raw[AD7606_CHANNEL_COUNT];

} ad7606_sample_t;


esp_err_t ad7606_driver_init(void);

void ad7606_driver_deinit(void);

esp_err_t ad7606_driver_read(
    ad7606_sample_t *sample
);

float ad7606_raw_to_uv(
    int16_t raw
);