#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef unsigned char DSTATUS;
typedef enum {
    RES_OK = 0,
    RES_ERROR,
    RES_WRPRT,
    RES_NOTRDY,
    RES_PARERR,
} DRESULT;

#define FF_DRV_NOT_USED 0xFF

typedef struct {
    DSTATUS (*init)(BYTE pdrv);
    DSTATUS (*status)(BYTE pdrv);
    DRESULT (*read)(BYTE pdrv, BYTE *buff, uint32_t sector, unsigned count);
    DRESULT (*write)(BYTE pdrv, const BYTE *buff, uint32_t sector, unsigned count);
    DRESULT (*ioctl)(BYTE pdrv, BYTE command, void *buffer);
} ff_diskio_impl_t;

void ff_diskio_register(BYTE pdrv, const ff_diskio_impl_t *impl);
