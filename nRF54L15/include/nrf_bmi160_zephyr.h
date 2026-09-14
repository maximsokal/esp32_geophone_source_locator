#ifndef NRF_BMI160_ZEPHYR_H
#define NRF_BMI160_ZEPHYR_H

#include "nrf_bmi160.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare the optional REMOTE-node BMI160 on the private D11/D12 I2C bus.
 * This function performs sensor I/O and must only be called after runtime role
 * assignment selects REMOTE. It is intentionally not a Zephyr boot device.
 */
nrf_bmi160_result_t nrf_bmi160_zephyr_init_remote(nrf_bmi160_t *device);

#ifdef __cplusplus
}
#endif

#endif
