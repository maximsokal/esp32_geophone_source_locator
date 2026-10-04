#pragma once

#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define SDMMC_FREQ_DEFAULT 20000
#define SDMMC_HOST_FLAG_DEINIT_ARG 0x01U

typedef struct {
    uint32_t capacity;
    uint16_t sector_size;
} sdmmc_csd_t;

typedef struct {
    uint32_t flags;
    int slot;
    int max_freq_khz;
    esp_err_t (*init)(void);
    esp_err_t (*deinit)(void);
    esp_err_t (*deinit_p)(int slot);
} sdmmc_host_t;

typedef struct sdmmc_card_t {
    sdmmc_csd_t csd;
    uint32_t max_freq_khz;
    int real_freq_khz;
} sdmmc_card_t;

esp_err_t fake_sdspi_host_init(void);
esp_err_t sdspi_host_remove_device(int slot);
esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card);
void sdmmc_card_print_info(FILE *stream, const sdmmc_card_t *card);

#define SDSPI_HOST_DEFAULT() ((sdmmc_host_t){ \
    .flags = SDMMC_HOST_FLAG_DEINIT_ARG, \
    .slot = 0, \
    .max_freq_khz = SDMMC_FREQ_DEFAULT, \
    .init = fake_sdspi_host_init, \
    .deinit = NULL, \
    .deinit_p = sdspi_host_remove_device, \
})
