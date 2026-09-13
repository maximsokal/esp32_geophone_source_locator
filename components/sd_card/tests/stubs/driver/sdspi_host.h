#pragma once

#include "sdmmc_cmd.h"


typedef struct {
    int slot;
    int max_freq_khz;
} sdmmc_host_t;

typedef struct {
    int host_id;
    int gpio_cs;
} sdspi_device_config_t;

#define SDSPI_HOST_DEFAULT() \
    ((sdmmc_host_t){ .slot = -1, .max_freq_khz = SDMMC_FREQ_DEFAULT })

#define SDSPI_DEVICE_CONFIG_DEFAULT() \
    ((sdspi_device_config_t){ .host_id = -1, .gpio_cs = -1 })
