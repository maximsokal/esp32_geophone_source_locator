#pragma once

#include <stdint.h>

#include "esp_err.h"


#define AD7606_CHANNEL_COUNT          8
#define AD7606_BUS_CLOCK_HZ     1000000U
#define AD7606_SPI_MODE                2U
#define AD7606_INPUT_RANGE_V           5.0f
#define AD7606_OVERSAMPLING_RATIO      1U


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
