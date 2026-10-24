#include "cjson_alloc_guard.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#if defined(ESP_PLATFORM)
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#endif

static _Atomic uint32_t s_failures;
static atomic_uintptr_t s_allocator;

#if defined(ESP_PLATFORM)
static portMUX_TYPE s_install_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_installed;
#endif

void cjson_alloc_guard_note_failure(void)
{
    atomic_fetch_add_explicit(&s_failures, 1U, memory_order_relaxed);
}

uint32_t cjson_alloc_guard_failures(void)
{
    return atomic_load_explicit(&s_failures, memory_order_relaxed);
}

void cjson_alloc_guard_set_allocator(cjson_alloc_guard_alloc_fn allocator)
{
    atomic_store_explicit(&s_allocator, (uintptr_t)allocator, memory_order_release);
}

void *cjson_alloc_guard_alloc(size_t size)
{
    cjson_alloc_guard_alloc_fn allocator = (cjson_alloc_guard_alloc_fn)
        atomic_load_explicit(&s_allocator, memory_order_acquire);
    void *allocation = allocator != NULL ? allocator(size) : malloc(size);
    if (allocation == NULL && size != 0U) {
        cjson_alloc_guard_note_failure();
    }
    return allocation;
}

void cjson_alloc_guard_install(void)
{
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_install_lock);
    if (!s_installed) {
        cJSON_Hooks hooks = {
            .malloc_fn = cjson_alloc_guard_alloc,
            .free_fn = free,
        };
        cJSON_InitHooks(&hooks);
        s_installed = true;
    }
    portEXIT_CRITICAL(&s_install_lock);
#endif
}
