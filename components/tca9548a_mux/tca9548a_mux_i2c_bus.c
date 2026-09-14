#include "tca9548a_mux_i2c_bus.h"

#include <stddef.h>

static esp_err_t tca9548a_i2c_write(void *context, uint8_t value)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_write_byte(
        (i2c_bus_device_handle_t)context,
        NULL_I2C_MEM_ADDR,
        value
    );
}

static esp_err_t tca9548a_i2c_read(void *context, uint8_t *value)
{
    if (context == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_read_byte(
        (i2c_bus_device_handle_t)context,
        NULL_I2C_MEM_ADDR,
        value
    );
}

esp_err_t tca9548a_mux_bind_i2c_bus(
    tca9548a_mux_t *mux,
    i2c_bus_device_handle_t device
)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const tca9548a_mux_io_t io = {
        .write_control = tca9548a_i2c_write,
        .read_control = tca9548a_i2c_read,
        .context = device,
    };
    return tca9548a_mux_init(mux, &io);
}
