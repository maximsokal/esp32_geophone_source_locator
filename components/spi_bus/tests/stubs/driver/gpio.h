#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

/* Mirror ESP-IDF's bitmask mode semantics: a mode without GPIO_MODE_DEF_INPUT
 * has its input path disabled, so gpio_get_level() cannot read it back. */
#define GPIO_MODE_DEF_INPUT   (1 << 0)
#define GPIO_MODE_DEF_OUTPUT  (1 << 1)

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = GPIO_MODE_DEF_INPUT,
    GPIO_MODE_OUTPUT = GPIO_MODE_DEF_OUTPUT,
    GPIO_MODE_INPUT_OUTPUT = (GPIO_MODE_DEF_INPUT | GPIO_MODE_DEF_OUTPUT),
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_17 17

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
