#ifndef NRF_SENSOR_BUS_H
#define NRF_SENSOR_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_SENSOR_BUS_OK = 0,
    NRF_SENSOR_BUS_ERR_ARGUMENT = -1,
    NRF_SENSOR_BUS_ERR_ADDRESS = -2,
    NRF_SENSOR_BUS_ERR_TRANSFER = -3,
} nrf_sensor_bus_result_t;

typedef int (*nrf_sensor_bus_transfer_fn)(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len);

typedef struct {
    nrf_sensor_bus_transfer_fn transfer;
    void *context;
} nrf_sensor_bus_t;

bool nrf_sensor_bus_is_valid_address(uint8_t address);

nrf_sensor_bus_result_t nrf_sensor_bus_write(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *data,
    size_t data_len);

nrf_sensor_bus_result_t nrf_sensor_bus_write_read(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len);

#ifdef __cplusplus
}
#endif

#endif
