#pragma once

#include <stddef.h>
#include <stdint.h>

struct device;
#define I2C_SPEED_STANDARD 1U
#define I2C_SPEED_FAST 2U
#define I2C_SPEED_SET(speed) ((speed) << 1U)
#define I2C_MODE_CONTROLLER (1U << 4U)
int i2c_configure(const struct device *device, uint32_t config);
int i2c_write_read(const struct device *device, uint16_t address,
    const void *write_data, size_t write_len, void *read_data, size_t read_len);
int i2c_write(const struct device *device, const uint8_t *data,
    uint32_t length, uint16_t address);
int i2c_read(const struct device *device, uint8_t *data,
    uint32_t length, uint16_t address);
