#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define SDMMC_FREQ_DEFAULT 20000

typedef struct {
    uint32_t max_freq_khz;
    int real_freq_khz;
    uint32_t is_mem : 1;
    uint32_t is_sdio : 1;
    uint32_t is_mmc : 1;
} sdmmc_card_t;

void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card);
esp_err_t sdmmc_get_status(sdmmc_card_t *card);
