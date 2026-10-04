#pragma once

#include "diskio_impl.h"

typedef struct {
    int unused;
} FATFS;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR = 1,
    FR_INT_ERR = 2,
    FR_NO_FILESYSTEM = 13,
} FRESULT;

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt);
