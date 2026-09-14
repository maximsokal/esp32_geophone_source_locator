#ifndef NRF_UPSTREAM_TARGET_ZEPHYR_H
#define NRF_UPSTREAM_TARGET_ZEPHYR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct i2c_target_callbacks;

/*
 * Platform adapter for the XIAO nRF54L15 upstream D4/D5 I2C target.
 *
 * This module intentionally does not define project I2C addresses or a wire
 * protocol. The caller owns both. Addresses passed here are raw 7-bit target
 * addresses and callbacks are the future protocol implementation.
 */
int nrf_upstream_target_zephyr_init(const struct i2c_target_callbacks *callbacks);
int nrf_upstream_target_zephyr_register(uint16_t address);
int nrf_upstream_target_zephyr_unregister(void);
int nrf_upstream_target_zephyr_readdress(uint16_t address);

/* Read Zephyr's raw hardware device ID. It is stable hardware identity data,
 * but the generic Zephyr API does not guarantee global uniqueness.
 */
int nrf_upstream_target_zephyr_read_device_id(
    uint8_t *buffer,
    size_t capacity,
    size_t *out_length);

bool nrf_upstream_target_zephyr_is_registered(void);
uint16_t nrf_upstream_target_zephyr_address(void);

#ifdef __cplusplus
}
#endif

#endif
