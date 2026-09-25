#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MALLOC_CAP_SPIRAM  (1U << 10)
#define MALLOC_CAP_8BIT    (1U << 1)

static inline void *heap_caps_malloc(size_t size, unsigned capabilities)
{
    (void)capabilities;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t count, size_t size, unsigned capabilities)
{
    (void)capabilities;
    return calloc(count, size);
}

static inline void heap_caps_free(void *pointer)
{
    free(pointer);
}
