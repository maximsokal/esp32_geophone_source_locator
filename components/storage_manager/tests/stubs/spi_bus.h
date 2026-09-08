#pragma once

#include "esp_err.h"

esp_err_t app_spi_bus_sd_operation_begin(void);
void app_spi_bus_sd_operation_end(void);
