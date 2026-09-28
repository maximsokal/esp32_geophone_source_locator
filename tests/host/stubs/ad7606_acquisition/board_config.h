#pragma once

#define BOARD_AD7606_CONVST_GPIO 42
#define BOARD_AD7606_BUSY_GPIO 47

esp_err_t board_gpio_isr_service_install(void);
