#ifndef ZEPHYR_DRIVERS_I2C_H
#define ZEPHYR_DRIVERS_I2C_H

#include <stdint.h>

struct i2c_target_config;

typedef void (*i2c_target_buf_write_received_cb_t)(
    struct i2c_target_config *config,
    uint8_t *ptr,
    uint32_t len);
typedef int (*i2c_target_buf_read_requested_cb_t)(
    struct i2c_target_config *config,
    uint8_t **ptr,
    uint32_t *len);

struct i2c_target_callbacks {
    i2c_target_buf_write_received_cb_t buf_write_received;
    i2c_target_buf_read_requested_cb_t buf_read_requested;
};

struct i2c_target_config {
    unsigned char node_stub;
    uint8_t flags;
    uint16_t address;
    const struct i2c_target_callbacks *callbacks;
};

struct device;
int i2c_target_register(const struct device *device, struct i2c_target_config *config);
int i2c_target_unregister(const struct device *device, struct i2c_target_config *config);

#endif
