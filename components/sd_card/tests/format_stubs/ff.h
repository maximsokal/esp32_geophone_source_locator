#pragma once

#include <stdint.h>

#include "diskio_impl.h"

typedef uint64_t LBA_t;
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR = 1,
    FR_NOT_READY = 3,
    FR_MKFS_ABORTED = 14,
} FRESULT;

typedef struct {
    BYTE fmt;
    BYTE n_fat;
    WORD n_root;
    DWORD align;
    DWORD au_size;
} MKFS_PARM;

#define FM_FAT32 0x02
#define CTRL_SYNC 0
#define GET_SECTOR_COUNT 1
#define GET_SECTOR_SIZE 2
#define GET_BLOCK_SIZE 3
#define CTRL_TRIM 4
#define STA_NOINIT 1

FRESULT f_fdisk(BYTE pdrv, const LBA_t partition_list[], void *workbuf);
FRESULT f_mkfs(const char *path, const MKFS_PARM *options, void *workbuf, UINT workbuf_size);
