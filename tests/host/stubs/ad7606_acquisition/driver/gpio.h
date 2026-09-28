#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;
typedef void (*gpio_isr_t)(void *);

typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE
} gpio_int_type_t;

#define ESP_INTR_FLAG_IRAM 0x01

esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
int gpio_get_level(gpio_num_t gpio_num);
