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

#if !defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER) || !CONFIG_MBEDTLS_DYNAMIC_BUFFER
#error "GeoLocator HTTPS requires CONFIG_MBEDTLS_DYNAMIC_BUFFER=y so TLS buffers can follow the external-memory policy. Regenerate sdkconfig from project defaults."
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

/*
 * Chrome opens multiple HTTPS sockets over the lifetime of the dashboard.
 * Session tickets allow successful clients to resume TLS 1.2 without doing a
 * full RSA certificate handshake every time. Both mbedTLS support and the
 * ESP-TLS server wrapper must be enabled for httpd_ssl_config_t::session_tickets.
 */
#if !defined(CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS) || !CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS
#error "GeoLocator HTTPS requires mbedTLS server session-ticket support. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_ESP_TLS_SERVER_SESSION_TICKETS) || !CONFIG_ESP_TLS_SERVER_SESSION_TICKETS
#error "GeoLocator HTTPS requires ESP-TLS server session tickets. Regenerate sdkconfig from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_SSL_RENEGOTIATION) && CONFIG_MBEDTLS_SSL_RENEGOTIATION
#error "GeoLocator disables legacy TLS renegotiation. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) || !CONFIG_FREERTOS_USE_TRACE_FACILITY
#error "GeoLocator runtime observability requires CONFIG_FREERTOS_USE_TRACE_FACILITY=y. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) || !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "GeoLocator runtime observability requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS) || !CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
#error "GeoLocator runtime observability requires FreeRTOS stats support. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID) || !CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
#error "GeoLocator runtime observability requires CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64) || !CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64
#error "GeoLocator runtime observability requires CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64=y. Regenerate sdkconfig from project defaults."
#endif

#if !defined(CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER) || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "GeoLocator runtime observability requires CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y. Regenerate sdkconfig from project defaults."
#endif

/* Keep a real translation unit so the guard is always compiled. */
void app_build_config_guard_link_anchor(void)
{
}
