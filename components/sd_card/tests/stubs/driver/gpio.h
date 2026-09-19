#pragma once

#include <stdint.h>

#include "esp_err.h"


typedef int gpio_num_t;

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

/* Mirror ESP-IDF's bitmask mode semantics: a mode without GPIO_MODE_DEF_INPUT
 * has its input path disabled, so gpio_get_level() cannot read it back. */
#define GPIO_MODE_DEF_INPUT   (1 << 0)
#define GPIO_MODE_DEF_OUTPUT  (1 << 1)

#define GPIO_MODE_DISABLE     0
#define GPIO_MODE_INPUT       GPIO_MODE_DEF_INPUT
#define GPIO_MODE_OUTPUT      GPIO_MODE_DEF_OUTPUT
#define GPIO_MODE_INPUT_OUTPUT (GPIO_MODE_DEF_INPUT | GPIO_MODE_DEF_OUTPUT)

#define GPIO_PULLUP_DISABLE   0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE     0

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, int level);
int gpio_get_level(gpio_num_t gpio_num);
