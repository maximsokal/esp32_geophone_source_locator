#pragma once

#include <stddef.h>

/*
 * On ESP firmware this installs process-wide cJSON hooks, so cJSON allocations
 * throughout the firmware prefer PSRAM and fall back to internal 8-bit RAM.
 * JSON trees are CPU-side data and are not DMA targets.
 */
#if defined(ESP_PLATFORM)
void web_cjson_install_psram_hooks(void);
#else
static inline void web_cjson_install_psram_hooks(void)
{
}
#endif

typedef void *(*web_cjson_alloc_fn)(size_t size);

/* Passes size through unchanged, including zero; each allocator may be NULL. */
void *web_cjson_alloc_with_fallback(
    size_t size,
    web_cjson_alloc_fn alloc_psram,
    web_cjson_alloc_fn alloc_internal
);
