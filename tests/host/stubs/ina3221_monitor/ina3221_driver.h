#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ina3221_policy.h"

typedef struct {
    bool ready;
} ina3221_driver_t;

esp_err_t ina3221_driver_open(ina3221_driver_t *driver);
void ina3221_driver_close(ina3221_driver_t *driver);
esp_err_t ina3221_driver_read_channels(
    ina3221_driver_t *driver,
    ina3221_channel_sample_t channels[INA3221_POLICY_CHANNEL_COUNT]
);
