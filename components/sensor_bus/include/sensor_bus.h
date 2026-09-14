#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "i2c_bus.h"

#define SENSOR_BUS_I2C_SPEED_HZ 100000U

typedef struct {
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t ina3221;
} sensor_bus_handles_t;

esp_err_t sensor_bus_init(void);
esp_err_t sensor_bus_get_handles(sensor_bus_handles_t *handles);
esp_err_t sensor_bus_get_tca9548a(i2c_bus_device_handle_t *device, uint8_t *address);
esp_err_t sensor_bus_deinit(void);
