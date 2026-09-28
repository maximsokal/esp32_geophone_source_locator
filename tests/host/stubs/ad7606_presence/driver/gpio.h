#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_47 47
#define ESP_INTR_FLAG_IRAM 0x01

typedef enum { GPIO_PULLUP_ONLY = 0, GPIO_PULLDOWN_ONLY = 1, GPIO_PULLUP_PULLDOWN = 2, GPIO_FLOATING = 3 } gpio_pull_mode_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE = 1, GPIO_INTR_NEGEDGE = 2, GPIO_INTR_ANYEDGE = 3 } gpio_int_type_t;
typedef void (*gpio_isr_t)(void *argument);

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull);
esp_err_t gpio_install_isr_service(int intr_alloc_flags);
esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num);
esp_err_t gpio_intr_enable(gpio_num_t gpio_num);
esp_err_t gpio_intr_disable(gpio_num_t gpio_num);
int gpio_get_level(gpio_num_t gpio_num);
