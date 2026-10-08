#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "cJSON.h"
#include "cjson_counting_alloc.h"

typedef union {
    struct {
        size_t requested_size;
        uint64_t magic;
        bool is_node;
    } metadata;
    double align_double;
    long long align_long_long;
    void *align_pointer;
    long double align_long_double;
} cjson_counting_header_t;

#define CJSON_COUNTING_ALLOC_MAGIC UINT64_C(0xc10a110ca110ca7e)

static cjson_counting_alloc_stats_t s_stats;
static size_t s_live_bytes_limit;
static uint64_t s_fail_index;

static size_t cjson_counting_bucket_for_size(size_t size)
{
    if (size <= 16U) return 0U;
    if (size <= 32U) return 1U;
    if (size <= 64U) return 2U;
    if (size <= 128U) return 3U;
    if (size <= 256U) return 4U;
    if (size <= 512U) return 5U;
    if (size <= 1024U) return 6U;
    if (size <= 2048U) return 7U;
    if (size <= 4096U) return 8U;
    if (size <= 8192U) return 9U;
    return 10U;
}

static void cjson_counting_record_request(size_t size)
{
    const size_t bucket = cjson_counting_bucket_for_size(size);
    s_stats.requested_bytes_total += (uint64_t)size;
    ++s_stats.histogram[bucket];
    if (size > s_stats.largest_request) s_stats.largest_request = size;
}

static bool cjson_counting_refuse_request(size_t size)
{
    ++s_stats.alloc_call_index;
    if (s_fail_index != 0U && s_stats.alloc_call_index == s_fail_index) {
        s_fail_index = 0U;
        ++s_stats.failures_injected;
        return true;
    }
    if (
        s_live_bytes_limit != 0U &&
        (s_stats.live_bytes > s_live_bytes_limit ||
         size > s_live_bytes_limit - s_stats.live_bytes)
    ) {
        ++s_stats.failures_injected;
        return true;
    }
    return false;
}

static void cjson_counting_update_peaks(void)
{
    if (s_stats.live_bytes > s_stats.peak_live_bytes) {
        s_stats.peak_live_bytes = s_stats.live_bytes;
    }
    if (s_stats.live_small_bytes > s_stats.peak_small_bytes) {
        s_stats.peak_small_bytes = s_stats.live_small_bytes;
    }
    if (s_stats.live_large_bytes > s_stats.peak_large_bytes) {
        s_stats.peak_large_bytes = s_stats.live_large_bytes;
    }
}

static cjson_counting_header_t *cjson_counting_allocate_block(
    size_t size,
    bool is_node,
    bool count_as_created
)
{
    if (size > SIZE_MAX - sizeof(cjson_counting_header_t)) return NULL;

    cjson_counting_header_t *header =
        malloc(sizeof(cjson_counting_header_t) + size);
    if (header == NULL) return NULL;

    header->metadata.requested_size = size;
    header->metadata.magic = CJSON_COUNTING_ALLOC_MAGIC;
    header->metadata.is_node = is_node;
    ++s_stats.live_blocks;
    s_stats.live_bytes += size;
    if (size <= CJSON_COUNTING_ALLOC_SMALL_MAX) {
        s_stats.live_small_bytes += size;
    } else {
        s_stats.live_large_bytes += size;
    }
    if (count_as_created) ++s_stats.blocks_allocated;
    cjson_counting_update_peaks();
    return header;
}

static void cjson_counting_release_block(
    cjson_counting_header_t *header,
    bool count_as_free_call,
    bool count_as_freed
)
{
    assert(header != NULL);
    assert(header->metadata.magic == CJSON_COUNTING_ALLOC_MAGIC);
    const size_t size = header->metadata.requested_size;
    if (count_as_free_call) ++s_stats.free_calls;
    if (count_as_freed) ++s_stats.blocks_freed;
    assert(s_stats.live_blocks > 0U);
    assert(s_stats.live_bytes >= size);
    --s_stats.live_blocks;
    s_stats.live_bytes -= size;
    if (size <= CJSON_COUNTING_ALLOC_SMALL_MAX) {
        assert(s_stats.live_small_bytes >= size);
        s_stats.live_small_bytes -= size;
    } else {
        assert(s_stats.live_large_bytes >= size);
        s_stats.live_large_bytes -= size;
    }
    header->metadata.magic = 0U;
    free(header);
}

void *cjson_counting_malloc(size_t size)
{
    ++s_stats.malloc_calls;
    cjson_counting_record_request(size);
    if (cjson_counting_refuse_request(size)) return NULL;

    cjson_counting_header_t *header =
        cjson_counting_allocate_block(size, false, true);
    if (header == NULL) return NULL;
    return (unsigned char *)header + sizeof(*header);
}

void *cjson_counting_calloc(size_t count, size_t size)
{
    size_t requested_size = 0U;
    const bool size_valid = size == 0U || count <= SIZE_MAX / size;
    if (size_valid) {
        requested_size = count * size;
    } else {
        requested_size = SIZE_MAX;
    }

    ++s_stats.calloc_calls;
    cjson_counting_record_request(requested_size);
    if (cjson_counting_refuse_request(requested_size) || !size_valid) return NULL;

    cjson_counting_header_t *header =
        cjson_counting_allocate_block(requested_size, true, true);
    if (header == NULL) return NULL;
    void *payload = (unsigned char *)header + sizeof(*header);
    memset(payload, 0, requested_size);
    return payload;
}

void *cjson_counting_realloc(void *pointer, size_t size)
{
    ++s_stats.realloc_calls;
    if (pointer != NULL && size == 0U) {
        cjson_counting_header_t *old_header =
            (cjson_counting_header_t *)((unsigned char *)pointer - sizeof(cjson_counting_header_t));
        cjson_counting_release_block(old_header, false, true);
        return NULL;
    }

    cjson_counting_record_request(size);
    if (cjson_counting_refuse_request(size)) return NULL;

    if (pointer == NULL) {
        cjson_counting_header_t *header =
            cjson_counting_allocate_block(size, false, true);
        if (header == NULL) return NULL;
        return (unsigned char *)header + sizeof(*header);
    }

    cjson_counting_header_t *old_header =
        (cjson_counting_header_t *)((unsigned char *)pointer - sizeof(cjson_counting_header_t));
    assert(old_header->metadata.magic == CJSON_COUNTING_ALLOC_MAGIC);
    const size_t old_size = old_header->metadata.requested_size;
    const bool was_node = old_header->metadata.is_node;

    /* Keep both blocks live during growth; a real heap may extend in place. */
    cjson_counting_header_t *new_header =
        cjson_counting_allocate_block(size, was_node, false);
    if (new_header == NULL) return NULL;
    const size_t copy_size = old_size < size ? old_size : size;
    void *new_payload = (unsigned char *)new_header + sizeof(*new_header);
    memcpy(new_payload, pointer, copy_size);
    cjson_counting_release_block(old_header, false, false);
    return new_payload;
}

void cjson_counting_free(void *pointer)
{
    if (pointer == NULL) return;
    cjson_counting_header_t *header =
        (cjson_counting_header_t *)((unsigned char *)pointer - sizeof(cjson_counting_header_t));
    cjson_counting_release_block(header, true, true);
}

void cjson_counting_alloc_get_stats(cjson_counting_alloc_stats_t *out)
{
    assert(out != NULL);
    *out = s_stats;
}

void cjson_counting_alloc_reset_counters(void)
{
    const size_t live_blocks = s_stats.live_blocks;
    const size_t live_bytes = s_stats.live_bytes;
    const size_t live_small_bytes = s_stats.live_small_bytes;
    const size_t live_large_bytes = s_stats.live_large_bytes;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.live_blocks = live_blocks;
    s_stats.live_bytes = live_bytes;
    s_stats.peak_live_bytes = live_bytes;
    s_stats.live_small_bytes = live_small_bytes;
    s_stats.peak_small_bytes = live_small_bytes;
    s_stats.live_large_bytes = live_large_bytes;
    s_stats.peak_large_bytes = live_large_bytes;
}

void cjson_counting_alloc_set_limit(size_t live_bytes_limit)
{
    s_live_bytes_limit = live_bytes_limit;
}

void cjson_counting_alloc_set_fail_index(uint64_t index)
{
    s_fail_index = index;
}

size_t cjson_counting_alloc_small_max(void)
{
    return CJSON_COUNTING_ALLOC_SMALL_MAX;
}

#define malloc cjson_counting_malloc
#define calloc cjson_counting_calloc
#define realloc cjson_counting_realloc
#define free cjson_counting_free
#include "../object_store_profile/cJSON.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

size_t cjson_counting_alloc_node_size(void)
{
    return sizeof(cJSON);
}
