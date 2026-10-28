#pragma once

#include <stdint.h>

/* Host stub of the nrfx GPIO HAL: only what the sensor bus needs to read the two I2C lines. */
#define NRF_GPIO_PIN_MAP(port, pin) ((((uint32_t)(port)) << 5) | ((uint32_t)(pin) & 0x1FU))

uint32_t nrf_gpio_pin_read(uint32_t pin_number);
