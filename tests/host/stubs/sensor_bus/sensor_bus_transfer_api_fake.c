#include "sensor_bus.h"
#include "i2c_bus.h"

esp_err_t sensor_bus_read_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    uint8_t *data)
{
    return i2c_bus_read_bytes(device, mem_address, data_len, data);
}

esp_err_t sensor_bus_write_bytes(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    size_t data_len,
    const uint8_t *data)
{
    return i2c_bus_write_bytes(device, mem_address, data_len, data);
}
