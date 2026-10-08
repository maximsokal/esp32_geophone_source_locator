#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t capacity;
    uint16_t sector_size;
} sdmmc_csd_t;

typedef struct sdmmc_card_t {
    sdmmc_csd_t csd;
} sdmmc_card_t;

esp_err_t sdmmc_get_status(sdmmc_card_t *card);
esp_err_t sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t sector, size_t count);
esp_err_t sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t sector, size_t count);
