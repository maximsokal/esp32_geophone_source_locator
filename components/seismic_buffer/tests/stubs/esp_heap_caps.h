#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_8BIT   (1U << 2)

#if defined(SEISMIC_BUFFER_TEST_ALLOCATOR)
void *heap_caps_malloc(size_t size, uint32_t caps);
void heap_caps_free(void *memory);
#else
static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *memory)
{
    free(memory);
}
#endif
