#include "sdkconfig.h"

/*
 * GeoLocator HTTPS is deliberately configured for the ESP32-S3 N16R8.
 *
 * sdkconfig.defaults files are only defaults: an old local sdkconfig has
 * higher priority and can silently keep mbedTLS in internal DRAM. On this
 * firmware that leaves too little contiguous internal heap for Chrome TLS
 * handshakes once Wi-Fi, LVGL and the web server are running.
 *
 * Fail the build instead of producing firmware which boots successfully but
 * later degrades into ERR_CONNECTION_RESET / TLS handshake failures.
 */
#if !defined(CONFIG_SPIRAM) || !CONFIG_SPIRAM
#error "GeoLocator requires PSRAM. Regenerate sdkconfig for target esp32s3."
#endif

#if !defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC) || !CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#error "GeoLocator HTTPS requires CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y. Delete stale sdkconfig and regenerate it from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC) && CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC
#error "GeoLocator HTTPS must not allocate mbedTLS state from internal DRAM. Regenerate sdkconfig from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_HARDWARE_AES) && CONFIG_MBEDTLS_HARDWARE_AES
#error "GeoLocator TLS policy requires software AES so PSRAM-backed mbedTLS does not fragment scarce internal DMA RAM. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) || !CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#error "GeoLocator requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y for HTTPS internal-RAM headroom. Regenerate sdkconfig from project defaults."
#endif

/*
 * SDSPI may need a temporary 512-byte MALLOC_CAP_DMA sector buffer. Browser
 * TLS sessions, Wi-Fi and display DMA all compete for internal-capable RAM, so
 * preserve a dedicated pool instead of letting ordinary malloc() consume it.
 */
#if !defined(CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) || (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL < 65536)
#error "GeoLocator requires CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL >= 65536 for SDSPI DMA headroom under HTTPS load. Regenerate sdkconfig from project defaults."
#endif

/*
 * A high malloc-always-internal threshold previously kept the object store's
 * 4 KiB JSON scratch buffers in internal RAM exactly while SDSPI needed DMA
 * memory. Keep only small ordinary allocations internal and prefer PSRAM for
 * larger scratch buffers.
 */
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) || (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL > 2048)
#error "GeoLocator requires CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL <= 2048 so large object/Web buffers prefer PSRAM. Regenerate sdkconfig from project defaults."
#endif

/* Keep a real translation unit so the guard is always compiled. */
void app_build_config_guard_link_anchor(void)
{
}
