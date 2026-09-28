#pragma once

#include "driver/gpio.h"

esp_err_t board_gpio_isr_service_install(void);

/* Must match components/board_config/include/board_config.h. */
#define BOARD_AD7606_BUSY_GPIO GPIO_NUM_47
