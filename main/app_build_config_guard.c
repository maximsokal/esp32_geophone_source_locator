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

/* Keep a real translation unit so the guard is always compiled. */
void app_build_config_guard_link_anchor(void)
{
}
