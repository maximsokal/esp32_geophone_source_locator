#pragma once

#include <stddef.h>

#define MALLOC_CAP_SPIRAM  0x01U
#define MALLOC_CAP_8BIT    0x02U

void *heap_caps_malloc(size_t size, unsigned capabilities);
void heap_caps_free(void *pointer);
