#include "sdkconfig.h"

/*
 * Storage paths intentionally use names longer than FAT 8.3 components
 * (for example "geolocator"). If LFN support is disabled, FatFs rejects
 * these paths with FR_INVALID_NAME, which the VFS surfaces as EINVAL.
 * Fail the firmware build instead of allowing a boot-looping image.
 */
#if !defined(CONFIG_FATFS_LFN_HEAP) && !defined(CONFIG_FATFS_LFN_STACK)
#error "storage_manager requires FAT long filename support (CONFIG_FATFS_LFN_HEAP or CONFIG_FATFS_LFN_STACK)"
#endif

#if !defined(CONFIG_FATFS_MAX_LFN) || (CONFIG_FATFS_MAX_LFN < 32)
#error "storage_manager requires CONFIG_FATFS_MAX_LFN >= 32"
#endif

/*
 * This board accesses the FAT volume through SDSPI while HTTPS/TLS is active.
 * If CONFIG_FATFS_ALLOC_PREFER_EXTRAM is enabled, FatFs work buffers prefer
 * PSRAM. ESP-IDF then has to allocate temporary MALLOC_CAP_DMA buffers for
 * sector I/O; those allocations can fail under TLS pressure and surface as
 * sdmmc_read_sectors ESP_ERR_NO_MEM / false object-store failures.
 */
#ifdef CONFIG_FATFS_ALLOC_PREFER_EXTRAM
#error "storage_manager requires CONFIG_FATFS_ALLOC_PREFER_EXTRAM=n for reliable SDSPI I/O under TLS load"
#endif
