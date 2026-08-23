#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

esp_err_t app_spi_bus_init(void);
spi_host_device_t app_spi_bus_get_host(void);