#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void *i2c_bus_handle_t;
typedef void *i2c_bus_device_handle_t;
typedef int i2c_port_t;

#define I2C_NUM_0 0
#define I2C_MODE_MASTER 1
#define GPIO_PULLUP_ENABLE 1
#define NULL_I2C_MEM_ADDR 0xFFU
#define NULL_I2C_DEV_ADDR 0xFFU

typedef struct {
    int mode;
    int sda_io_num;
    int sda_pullup_en;
    int scl_io_num;
    int scl_pullup_en;
    struct {
        uint32_t clk_speed;
    } master;
} i2c_config_t;

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, const i2c_config_t *config);
i2c_bus_device_handle_t i2c_bus_device_create(i2c_bus_handle_t bus, uint8_t address, uint32_t speed_hz);
esp_err_t i2c_bus_device_delete(i2c_bus_device_handle_t *device);
esp_err_t i2c_bus_delete(i2c_bus_handle_t *bus);
esp_err_t i2c_bus_read_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t *data);
esp_err_t i2c_bus_write_byte(i2c_bus_device_handle_t device, uint8_t mem_address, uint8_t data);
esp_err_t i2c_bus_read_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, uint8_t *data);
esp_err_t i2c_bus_write_bytes(i2c_bus_device_handle_t device, uint8_t mem_address, size_t data_len, const uint8_t *data);
