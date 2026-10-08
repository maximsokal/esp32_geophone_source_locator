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
#include <stdbool.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_cjson_hooks_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_cjson_hooks_installed;

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
    portENTER_CRITICAL(&s_cjson_hooks_lock);
    if (!s_cjson_hooks_installed) {
        cJSON_Hooks hooks = {
            .malloc_fn = web_cjson_malloc,
            .free_fn = free,
        };
        cJSON_InitHooks(&hooks);
        s_cjson_hooks_installed = true;
    }
    portEXIT_CRITICAL(&s_cjson_hooks_lock);
}
#endif
