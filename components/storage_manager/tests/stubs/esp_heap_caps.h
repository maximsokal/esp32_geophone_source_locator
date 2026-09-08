#pragma once

#include <stddef.h>

#define MALLOC_CAP_DMA 1U

size_t heap_caps_get_free_size(unsigned capabilities);
size_t heap_caps_get_largest_free_block(unsigned capabilities);
void *heap_caps_malloc(size_t size, unsigned capabilities);
void heap_caps_free(void *ptr);
