#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_SPIRAM  (1U << 10)
#define MALLOC_CAP_8BIT    (1U << 1)

void *localization_test_psram_malloc(size_t size);
void *localization_test_psram_calloc(size_t count, size_t size);
void localization_test_psram_free(void *pointer);
size_t localization_test_psram_largest_free_block(uint32_t caps);

static inline void *heap_caps_malloc(size_t size, uint32_t capabilities)
{
    (void)capabilities;
    return localization_test_psram_malloc(size);
}

static inline void *heap_caps_calloc(size_t count, size_t size, uint32_t capabilities)
{
    (void)capabilities;
    return localization_test_psram_calloc(count, size);
}

static inline void heap_caps_free(void *pointer)
{
    localization_test_psram_free(pointer);
}

static inline size_t heap_caps_get_largest_free_block(uint32_t capabilities)
{
    return localization_test_psram_largest_free_block(capabilities);
}
