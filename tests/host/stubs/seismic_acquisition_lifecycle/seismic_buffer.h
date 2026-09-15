#pragma once

#include <stdint.h>

#include "esp_err.h"

#define SEISMIC_SOURCE_NONE 0

esp_err_t seismic_buffer_configure(
    int source,
    uint32_t sample_rate_hz
);
