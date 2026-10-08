#pragma once

#include "esp_err.h"

#include "sdmmc_cmd.h"


typedef esp_err_t (*sd_card_internal_unmount_fn_t)(void);


/*
 * ESP-IDF's SDSPI default runtime ceiling is 20 MHz. Card discovery itself
 * is performed by the protocol layer at the probing clock before the driver
 * switches to this value. Keep the production target at the standard clock;
 * lowering it here would hide signal-integrity or power problems instead of
 * fixing them.
 */
#define SD_CARD_SPI_FREQ_KHZ  SDMMC_FREQ_DEFAULT


const char *sd_card_internal_mount_point(void);


sdmmc_card_t *sd_card_internal_card(void);


void sd_card_internal_set_card(
    sdmmc_card_t *card
);

void sd_card_internal_set_recovery_unmount(
    sd_card_internal_unmount_fn_t unmount_fn
);

sd_card_internal_unmount_fn_t sd_card_internal_recovery_unmount(void);


esp_err_t sd_card_internal_prepare_shared_spi(void);
