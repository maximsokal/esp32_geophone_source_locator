#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;

#define I2C_ADDR_BIT_LEN_7 7
#define I2C_CLK_SRC_DEFAULT 0

typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    unsigned int glitch_ignore_cnt;
    struct {
        unsigned int enable_internal_pullup : 1;
    } flags;
} i2c_master_bus_config_t;

esp_err_t i2c_new_master_bus(
    const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus);

typedef struct {
    int dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    struct {
        unsigned int disable_ack_check : 1;
    } flags;
} i2c_device_config_t;

i2c_master_bus_handle_t i2c_bus_get_internal_bus_handle(void *bus);
esp_err_t i2c_master_bus_add_device(
    i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config,
    i2c_master_dev_handle_t *device);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_receive(
    i2c_master_dev_handle_t device,
    uint8_t *data,
    size_t data_len,
    int timeout_ms);
esp_err_t i2c_master_transmit_receive(
    i2c_master_dev_handle_t device,
    const uint8_t *write_buffer,
    size_t write_size,
    uint8_t *read_buffer,
    size_t read_size,
    int timeout_ms);
esp_err_t i2c_master_transmit(
    i2c_master_dev_handle_t device,
    const uint8_t *data,
    size_t data_len,
    int timeout_ms);
