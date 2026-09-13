#pragma once

#include <stdint.h>
#include <stdio.h>

#define SDMMC_FREQ_DEFAULT 20000

typedef struct {
    uint32_t max_freq_khz;
    int real_freq_khz;
} sdmmc_card_t;

void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card);
