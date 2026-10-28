#ifndef NRF_BME280_ZEPHYR_H
#define NRF_BME280_ZEPHYR_H

#include "nrf_bme280.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the optional REMOTE BME280 over the private D11/D12 I2C bus.
 * This function is intentionally not called during generic MAIN/REMOTE boot;
 * invoke it only after runtime role assignment selects REMOTE.
 */
nrf_bme280_result_t nrf_bme280_zephyr_init_remote(nrf_bme280_t *device);
nrf_bme280_result_t nrf_bme280_zephyr_begin_remote(nrf_bme280_t *device);
nrf_bme280_result_t nrf_bme280_zephyr_step(nrf_bme280_t *device, uint64_t now_ms);
int nrf_bme280_zephyr_state(void);
void nrf_bme280_zephyr_note_recover(void);

#ifdef __cplusplus
}
#endif

#endif
