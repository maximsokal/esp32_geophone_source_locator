#pragma once

#include <stdbool.h>
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

/*
 * Physical presence is established from real BUSY LOW-HIGH-LOW conversion
 * pulses, not from SPI receiving a plausible constant byte pattern. This is
 * intentionally separate from transport initialization because an open MISO
 * line can still complete an SPI transaction.
 */
esp_err_t ad7606_driver_presence_monitor_start(void);
void ad7606_driver_presence_monitor_stop(void);
bool ad7606_driver_presence_is_healthy(void);

float ad7606_raw_to_uv(
    int16_t raw
);
