#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;

esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data);
esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data);
