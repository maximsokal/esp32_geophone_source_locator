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

/* Automatic diagnostics inspect only the four supported sensor addresses. */
int nrf_sensor_bus_zephyr_scan_expected(void);

/*
 * Diagnostic for a failed sensor initialisation: probes every 7-bit address on the private bus with
 * a one byte read and, for each address that answers, reads the ID registers the supported parts use
 * (0x00 BMI160, 0x75 MPU-style WHO_AM_I, 0xD0 BMP280/BME280). With CONFIG_APP_NRF_VERBOSE_LOG it prints
 * one "sensor_scan:" line per answering address. Returns the number of addresses that answered, or
 * a negative errno when the controller is not ready.
 */
int nrf_sensor_bus_zephyr_scan_private(void);
int nrf_sensor_bus_zephyr_scan_private_bitmap(uint32_t bitmap[4]);
/* Probe one address; returns 1 for ACK, 0 for NACK, or a negative errno. */
int nrf_sensor_bus_zephyr_scan_address(uint8_t address);

/*
 * Cause of the failed transfers to [first_address, last_address], for the diagnostic event. An address's record
 * starts at its first failure and ends at its next successful transfer or at this call, whichever comes first.
 * Only transfers made through the bus returned by init_private() are recorded; the scan helpers are not, so an
 * expected NACK from a probe does not look like a fault. Returns the highest-ranking sample (a line held low,
 * then timeout, then other errors, then EIO) with the failure count of its address, clears every recorded
 * address in the range, and returns false when none was recorded or `out` is NULL.
 */
bool nrf_sensor_bus_zephyr_take_fault(uint8_t first_address, uint8_t last_address, nrf_sensor_bus_fault_t *out);
/* take_fault() already packed for the SENSOR_BUS_FAULT event (nrf_sensor_bus_fault_pack() layout). */
bool nrf_sensor_bus_zephyr_take_fault_word(uint8_t first_address, uint8_t last_address, uint32_t *packed);
/* Failures that found no free slot in the fixed table (more than eight distinct addresses since the last take). */
uint32_t nrf_sensor_bus_zephyr_dropped_faults(void);

#ifdef __cplusplus
}
#endif

#endif
