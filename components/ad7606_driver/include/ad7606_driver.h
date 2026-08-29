#pragma once

#include <stdint.h>

#include "esp_err.h"


#define AD7606_CHANNEL_COUNT 8


typedef struct
{
    /*
     * Timestamp captured immediately after the CONVST rising edge
     * that starts the simultaneous conversion of all 8 channels.
     */
    int64_t timestamp_us;

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
