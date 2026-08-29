#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ad7606_driver.h"


typedef struct
{
    bool running;
    bool latest_valid;

    uint64_t samples;
    uint64_t read_errors;
    uint32_t consecutive_failures;

    int64_t last_sample_timestamp_us;

    float latest_uv[AD7606_CHANNEL_COUNT];

} ad7606_acquisition_status_t;


esp_err_t ad7606_acquisition_start(void);

void ad7606_acquisition_stop(void);

esp_err_t ad7606_acquisition_get_status(
    ad7606_acquisition_status_t *status
);
