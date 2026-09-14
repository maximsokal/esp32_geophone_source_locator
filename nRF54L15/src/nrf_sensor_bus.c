#include "nrf_sensor_bus.h"

static nrf_sensor_bus_result_t nrf_sensor_bus_validate(
    const nrf_sensor_bus_t *bus,
    uint8_t address)
{
    if (bus == NULL || bus->transfer == NULL) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }
    if (!nrf_sensor_bus_is_valid_address(address)) {
        return NRF_SENSOR_BUS_ERR_ADDRESS;
    }
    return NRF_SENSOR_BUS_OK;
}

bool nrf_sensor_bus_is_valid_address(uint8_t address)
{
    return address >= 0x08U && address <= 0x77U;
}

nrf_sensor_bus_result_t nrf_sensor_bus_write(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *data,
    size_t data_len)
{
    const nrf_sensor_bus_result_t validation =
        nrf_sensor_bus_validate(bus, address);
    if (validation != NRF_SENSOR_BUS_OK) {
        return validation;
    }
    if (data == NULL || data_len == 0U) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }

    return bus->transfer(
               bus->context,
               address,
               data,
               data_len,
               NULL,
               0U) == 0
               ? NRF_SENSOR_BUS_OK
               : NRF_SENSOR_BUS_ERR_TRANSFER;
}

nrf_sensor_bus_result_t nrf_sensor_bus_write_read(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    const nrf_sensor_bus_result_t validation =
        nrf_sensor_bus_validate(bus, address);
    if (validation != NRF_SENSOR_BUS_OK) {
        return validation;
    }
    if (write_data == NULL || write_len == 0U ||
        read_data == NULL || read_len == 0U) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }

    return bus->transfer(
               bus->context,
               address,
               write_data,
               write_len,
               read_data,
               read_len) == 0
               ? NRF_SENSOR_BUS_OK
               : NRF_SENSOR_BUS_ERR_TRANSFER;
}
