#pragma once

#include "esp_err.h"

#include "sdmmc_cmd.h"


#define SD_CARD_SPI_FREQ_KHZ  2000


const char *sd_card_internal_mount_point(void);


sdmmc_card_t *sd_card_internal_card(void);


void sd_card_internal_set_card(
    sdmmc_card_t *card
);


sdmmc_card_t *sd_card_internal_take_card(void);


esp_err_t sd_card_internal_prepare_shared_spi(void);
