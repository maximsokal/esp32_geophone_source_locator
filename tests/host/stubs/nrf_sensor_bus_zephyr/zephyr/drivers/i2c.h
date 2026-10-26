#pragma once

#include <stddef.h>
#include <stdint.h>

struct device;
int i2c_write_read(const struct device *device, uint16_t address,
    const void *write_data, size_t write_len, void *read_data, size_t read_len);
int i2c_write(const struct device *device, const uint8_t *data,
    uint32_t length, uint16_t address);
int i2c_read(const struct device *device, uint8_t *data,
    uint32_t length, uint16_t address);
