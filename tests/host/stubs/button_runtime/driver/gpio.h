#pragma once

#include "esp_err.h"
typedef int gpio_num_t;
esp_err_t gpio_intr_disable(gpio_num_t gpio);
