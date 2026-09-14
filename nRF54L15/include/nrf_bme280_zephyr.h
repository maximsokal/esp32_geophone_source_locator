#ifndef NRF_BME280_ZEPHYR_H
#define NRF_BME280_ZEPHYR_H

#include <stdbool.h>

#include "nrf_bme280.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the optional REMOTE BME280 over the private D11/D12 I2C bus.
 * This function is intentionally not called during generic MAIN/REMOTE boot;
 * invoke it only after runtime role assignment selects REMOTE.
 */
bool nrf_bme280_zephyr_init_remote(nrf_bme280_t *device);

#ifdef __cplusplus
}
#endif

#endif
