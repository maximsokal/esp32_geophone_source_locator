#include "sdkconfig.h"

/*
 * GeoLocator HTTPS is deliberately configured for the ESP32-S3 N16R8.
 *
 * sdkconfig.defaults files are only defaults: an old local sdkconfig has
 * higher priority and can silently keep an obsolete TLS memory policy.
 *
 * Target evidence showed that forcing every mbedTLS allocation into PSRAM and
 * using dynamic TLS buffers could corrupt the PSRAM TLSF heap after repeated
 * browser handshakes. Use ESP-IDF's default allocator with the project 2 KiB
 * threshold instead: small crypto/key allocations prefer internal DRAM and
 * large TLS buffers prefer PSRAM.
 *
 * Fail the build instead of producing firmware with a stale allocator policy.
 */
#if !defined(CONFIG_SPIRAM) || !CONFIG_SPIRAM
#error "GeoLocator requires PSRAM. Regenerate sdkconfig for target esp32s3."
#endif

#if !defined(CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC) || !CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC
#error "GeoLocator HTTPS requires CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC=y. Delete stale sdkconfig and regenerate it from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC) && CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
#error "GeoLocator HTTPS must not force all mbedTLS allocations into PSRAM; target handshakes corrupted the PSRAM TLSF heap. Regenerate sdkconfig from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC) && CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC
#error "GeoLocator HTTPS must use the default hybrid allocator, not internal-only mbedTLS allocation. Regenerate sdkconfig from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER) && CONFIG_MBEDTLS_DYNAMIC_BUFFER
#error "GeoLocator HTTPS disables CONFIG_MBEDTLS_DYNAMIC_BUFFER after target TLSF corruption under repeated browser handshakes. Regenerate sdkconfig from project defaults."
#endif

#if defined(CONFIG_MBEDTLS_HARDWARE_AES) && CONFIG_MBEDTLS_HARDWARE_AES
#error "GeoLocator TLS policy requires software AES so PSRAM-backed TLS buffers do not consume scarce internal DMA RAM. Regenerate sdkconfig from project defaults."
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
 * With CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC, this threshold is part of the TLS
 * policy: small RSA/MPI/session allocations stay internal while 4 KiB/16 KiB
 * buffers prefer PSRAM. Keep it at or below 2 KiB.
 */
#if !defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) || (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL > 2048)
#error "GeoLocator requires CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL <= 2048 so large object/Web/TLS buffers prefer PSRAM. Regenerate sdkconfig from project defaults."
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

/*
 * Object-store leases are the primary lifetime barrier. FatFs locking is a
 * second line of defense for the reviewed simultaneous-open-file budget.
 */
#if !defined(CONFIG_FATFS_FS_LOCK) || (CONFIG_FATFS_FS_LOCK < 7)
#error "GeoLocator requires CONFIG_FATFS_FS_LOCK >= 7. Build with tools/run_idf.ps1 or regenerate sdkconfig from project defaults."
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
