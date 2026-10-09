#pragma once
#include "i2c_bus.h"
#include "tca9548a_mux.h"
esp_err_t tca9548a_mux_bind_i2c_bus(
    tca9548a_mux_t *mux,
    i2c_bus_device_handle_t device,
    uint8_t address);
