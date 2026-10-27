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

/*
 * Diagnostic for a failed sensor initialisation: probes every 7-bit address on the private bus with
 * a one byte read and, for each address that answers, reads the ID registers the supported parts use
 * (0x00 BMI160, 0x75 MPU-style WHO_AM_I, 0xD0 BMP280/BME280). With CONFIG_APP_NRF_VERBOSE_LOG it prints
 * one "sensor_scan:" line per answering address. Returns the number of addresses that answered, or
 * a negative errno when the controller is not ready.
 */
int nrf_sensor_bus_zephyr_scan_private(void);

#ifdef __cplusplus
}
#endif

#endif
