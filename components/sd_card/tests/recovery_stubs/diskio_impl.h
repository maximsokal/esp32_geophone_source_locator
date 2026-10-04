#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef unsigned char BYTE;
#define FF_DRV_NOT_USED 0xFF

esp_err_t ff_diskio_get_drive(BYTE *out_pdrv);
void ff_diskio_unregister(BYTE pdrv);
