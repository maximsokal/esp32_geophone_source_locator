#ifndef NRF_LOGGER_LED_ZEPHYR_H
#define NRF_LOGGER_LED_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_logger_led.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_LOGGER_LED_ZEPHYR_OK = 0,
    NRF_LOGGER_LED_ZEPHYR_UNAVAILABLE = 1,
    NRF_LOGGER_LED_ZEPHYR_ERR_ARGUMENT = -1,
    NRF_LOGGER_LED_ZEPHYR_ERR_GPIO = -2,
    NRF_LOGGER_LED_ZEPHYR_ERR_NOT_INITIALIZED = -3,
} nrf_logger_led_zephyr_result_t;

typedef enum {
    NRF_LOGGER_LED_COLOR_GREEN = 0,
    NRF_LOGGER_LED_COLOR_YELLOW,
    NRF_LOGGER_LED_COLOR_RED,
} nrf_logger_led_color_t;

typedef enum {
    NRF_LOGGER_LED_PIN_INVALID = -1,
    NRF_LOGGER_LED_PIN_UNAVAILABLE = 0,
    NRF_LOGGER_LED_PIN_READY,
    NRF_LOGGER_LED_PIN_FAILED,
} nrf_logger_led_pin_state_t;

/* Configure each present logger LED inactive; missing pins are non-fatal. */
nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_init(void);
/* Apply logical outputs using each pin's DeviceTree polarity flags. */
nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_apply(
    const nrf_logger_led_output_t *output);
/* Boot diagnostics use the same DeviceTree specs and logical polarity path. */
nrf_logger_led_zephyr_result_t nrf_logger_led_zephyr_set_test_pin(
    nrf_logger_led_color_t color,
    bool on);
nrf_logger_led_pin_state_t nrf_logger_led_zephyr_pin_state(
    nrf_logger_led_color_t color);
uint32_t nrf_logger_led_zephyr_failure_count(void);
void nrf_logger_led_zephyr_latch_init_fatal(void);
bool nrf_logger_led_zephyr_init_fatal(void);

#ifdef __cplusplus
}
#endif

#endif
