#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_INTERNAL 4U
size_t heap_caps_get_free_size(unsigned caps);
#ifdef UI_NATIVE_FULL_SCREENS
void *render_heap_malloc(size_t size);
void *render_heap_calloc(size_t count, size_t size);
void render_heap_free(void *memory);
#define heap_caps_malloc(size, caps) ((void)(caps), render_heap_malloc(size))
#define heap_caps_calloc(count, size, caps) ((void)(caps), render_heap_calloc(count, size))
#define heap_caps_free render_heap_free
#else
#define heap_caps_malloc(size, caps) ((void)(caps), malloc(size))
#define heap_caps_calloc(count, size, caps) ((void)(caps), calloc(count, size))
#define heap_caps_free free
#endif
