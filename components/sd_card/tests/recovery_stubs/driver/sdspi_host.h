#pragma once

#include "board_config.h"
#include "sdmmc_cmd.h"

typedef struct {
    int host_id;
    int gpio_cs;
} sdspi_device_config_t;

#define SDSPI_DEVICE_CONFIG_DEFAULT() ((sdspi_device_config_t){ \
    .host_id = BOARD_SPI_HOST, \
    .gpio_cs = BOARD_SD_CS_GPIO, \
})

esp_err_t sdspi_host_init_device(const sdspi_device_config_t *config, int *out_handle);
