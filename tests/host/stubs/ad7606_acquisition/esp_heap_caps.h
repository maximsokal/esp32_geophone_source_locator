#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x01U
#define MALLOC_CAP_8BIT 0x02U

static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}
