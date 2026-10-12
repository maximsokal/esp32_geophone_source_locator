#include "sensor_bus.h"
#include "i2c_bus.h"

esp_err_t sensor_bus_device_create(
    i2c_bus_handle_t bus,
    uint8_t address,
    uint32_t speed_hz,
    i2c_bus_device_handle_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *device = i2c_bus_device_create(bus, address, speed_hz);
    return *device != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sensor_bus_device_delete(i2c_bus_device_handle_t *device)
{
    return i2c_bus_device_delete(device);
}

esp_err_t sensor_bus_read_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t *data)
{
    return i2c_bus_read_byte(device, mem_address, data);
}

esp_err_t sensor_bus_write_byte(
    i2c_bus_device_handle_t device,
    uint8_t mem_address,
    uint8_t data)
{
    return i2c_bus_write_byte(device, mem_address, data);
}

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
