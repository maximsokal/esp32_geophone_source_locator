#ifndef NRF_BLUE_LED_H
#define NRF_BLUE_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_verticality_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_BLUE_LED_OFF = 0,
    NRF_BLUE_LED_ON,
    NRF_BLUE_LED_BLINK_2HZ,
} nrf_blue_led_action_t;

#define NRF_BLUE_LED_BLINK_HALF_PERIOD_MS 250U

nrf_blue_led_action_t nrf_blue_led_policy(nrf_verticality_state_t state);

/* Zephyr adapter; both functions are called from the sensor worker. */
int nrf_blue_led_zephyr_init(void);
int nrf_blue_led_zephyr_set_test_pin(bool on);
int8_t nrf_blue_led_zephyr_state(void);
uint32_t nrf_blue_led_zephyr_failure_count(void);
void nrf_blue_led_zephyr_update(
    nrf_verticality_state_t state,
    uint64_t uptime_ms);

#ifdef __cplusplus
}
#endif

#endif
