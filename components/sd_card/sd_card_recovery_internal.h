#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"


bool sd_card_recovery_owns_card(const sdmmc_card_t *card);
int sd_card_recovery_drive_number(void);
esp_err_t sd_card_recovery_prepare_for_format(void);
void sd_card_recovery_restore_default_diskio(void);
esp_err_t sd_card_recovery_mount_filesystem(int *out_fresult);
