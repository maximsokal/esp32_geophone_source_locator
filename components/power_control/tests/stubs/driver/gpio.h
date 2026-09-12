#pragma once

typedef int gpio_num_t;

#define GPIO_NUM_18 18
#define GPIO_NUM_21 21

int gpio_get_level(gpio_num_t gpio_num);
