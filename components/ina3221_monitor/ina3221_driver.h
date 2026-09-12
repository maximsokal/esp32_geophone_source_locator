#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "ina3221_policy.h"
#include "sensor_bus.h"


typedef struct
{
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t device;
    bool ready;

} ina3221_driver_t;


esp_err_t ina3221_driver_open(
    ina3221_driver_t *driver
);

void ina3221_driver_close(
    ina3221_driver_t *driver
);

esp_err_t ina3221_driver_read_channels(
    ina3221_driver_t *driver,
    ina3221_channel_sample_t
        channels[INA3221_POLICY_CHANNEL_COUNT]
);
