#ifndef NRF_SENSOR_BUS_ZEPHYR_H
#define NRF_SENSOR_BUS_ZEPHYR_H

#include <stdbool.h>

#include "nrf_sensor_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bind the hardware-independent sensor-bus contract to the XIAO private
 * D11/D12 I2C controller. The pinned board definition maps those pins to
 * Zephyr node i2c30 (P0.04 SDA, P0.03 SCL).
 */
bool nrf_sensor_bus_zephyr_init_private(nrf_sensor_bus_t *out_bus);

#ifdef __cplusplus
}
#endif

#endif
