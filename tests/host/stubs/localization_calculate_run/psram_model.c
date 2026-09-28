#include "psram_model.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"


#define TEST_PSRAM_BYTES (8U * 1024U * 1024U)

typedef struct
{
    long double alignment;
    size_t size;
} psram_allocation_header_t;

static size_t s_reserved_bytes;
static size_t s_allocated_bytes;


static size_t psram_free_bytes(void)
{
    return TEST_PSRAM_BYTES - s_reserved_bytes - s_allocated_bytes;
}


void localization_test_psram_model_set_reserved(size_t reserved_bytes)
{
    assert(s_allocated_bytes == 0U);
    assert(reserved_bytes <= TEST_PSRAM_BYTES);
    s_reserved_bytes = reserved_bytes;
}


void localization_test_psram_model_reset(void)
{
    assert(s_allocated_bytes == 0U);
    s_reserved_bytes = 0U;
}


size_t localization_test_psram_model_free_bytes(void)
{
    return psram_free_bytes();
}


size_t localization_test_psram_model_allocated_bytes(void)
{
    return s_allocated_bytes;
}


void *localization_test_psram_malloc(size_t size)
{
    if (size == 0U || size > psram_free_bytes() ||
        size > SIZE_MAX - sizeof(psram_allocation_header_t)) {
        return NULL;
    }
    psram_allocation_header_t *header = malloc(
        sizeof(*header) + size
    );
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    s_allocated_bytes += size;
    return header + 1;
}


void *localization_test_psram_calloc(size_t count, size_t size)
{
    if (count != 0U && size > SIZE_MAX / count) {
        return NULL;
    }
    const size_t bytes = count * size;
    void *pointer = localization_test_psram_malloc(bytes);
    if (pointer != NULL) {
        memset(pointer, 0, bytes);
    }
    return pointer;
}


void localization_test_psram_free(void *pointer)
{
    if (pointer == NULL) {
        return;
    }
    psram_allocation_header_t *header =
        (psram_allocation_header_t *)pointer - 1;
    assert(header->size <= s_allocated_bytes);
    s_allocated_bytes -= header->size;
    free(header);
}


size_t localization_test_psram_largest_free_block(uint32_t caps)
{
    (void)caps;
    return psram_free_bytes();
}
