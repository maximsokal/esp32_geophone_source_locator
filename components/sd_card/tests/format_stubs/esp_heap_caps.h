#pragma once

#include <stddef.h>

#define MALLOC_CAP_INTERNAL 0x01U
#define MALLOC_CAP_DMA 0x02U
#define MALLOC_CAP_8BIT 0x04U
#define MALLOC_CAP_SPIRAM 0x08U

void *heap_caps_malloc(size_t size, unsigned caps);
void heap_caps_free(void *ptr);
