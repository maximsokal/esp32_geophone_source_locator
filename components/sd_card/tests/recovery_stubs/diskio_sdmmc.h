#pragma once

#include <stdbool.h>

#include "diskio_impl.h"
#include "sdmmc_cmd.h"

void ff_diskio_register_sdmmc(unsigned char pdrv, sdmmc_card_t *card);
void ff_sdmmc_set_disk_status_check(BYTE pdrv, bool enable);
