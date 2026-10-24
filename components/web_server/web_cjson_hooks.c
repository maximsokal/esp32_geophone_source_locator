#include "web_cjson_hooks.h"

void *web_cjson_alloc_with_fallback(
    size_t size,
    web_cjson_alloc_fn alloc_psram,
    web_cjson_alloc_fn alloc_internal
)
{
    void *allocation = alloc_psram != NULL ? alloc_psram(size) : NULL;
    if (allocation != NULL || alloc_internal == NULL) {
        return allocation;
    }
    return alloc_internal(size);
}

#if defined(ESP_PLATFORM)
#include "cjson_alloc_guard.h"
#include "esp_heap_caps.h"

static void *web_cjson_alloc_psram(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *web_cjson_alloc_internal(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void *web_cjson_malloc(size_t size)
{
    return web_cjson_alloc_with_fallback(
        size,
        web_cjson_alloc_psram,
        web_cjson_alloc_internal
    );
}

void web_cjson_install_psram_hooks(void)
{
    /* The guard owns the hooks (and counts allocation failures); this only
     * selects the PSRAM-preferred allocator behind them. */
    cjson_alloc_guard_install();
    cjson_alloc_guard_set_allocator(web_cjson_malloc);
}
#endif
