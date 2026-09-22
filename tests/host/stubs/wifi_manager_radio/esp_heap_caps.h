#pragma once

#define MALLOC_CAP_DMA       1U
#define MALLOC_CAP_INTERNAL  2U

static inline unsigned heap_caps_get_free_size(unsigned caps)
{
    (void)caps;
    return 12345U;
}

static inline unsigned heap_caps_get_largest_free_block(unsigned caps)
{
    (void)caps;
    return 6789U;
}
