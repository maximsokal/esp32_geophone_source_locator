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
