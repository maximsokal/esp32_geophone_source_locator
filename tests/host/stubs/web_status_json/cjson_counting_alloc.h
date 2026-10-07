#ifndef CJSON_COUNTING_ALLOC_H
#define CJSON_COUNTING_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#define CJSON_COUNTING_ALLOC_SMALL_MAX 2048U
#define CJSON_COUNTING_ALLOC_HISTOGRAM_BUCKETS 11U

typedef struct {
    uint64_t malloc_calls;
    uint64_t calloc_calls;
    uint64_t realloc_calls;
    uint64_t free_calls;
    uint64_t blocks_allocated;
    uint64_t blocks_freed;
    uint64_t requested_bytes_total;
    uint64_t failures_injected;
    uint64_t alloc_call_index;
    size_t live_blocks;
    size_t live_bytes;
    size_t peak_live_bytes;
    size_t live_small_bytes;
    size_t peak_small_bytes;
    size_t live_large_bytes;
    size_t peak_large_bytes;
    uint64_t histogram[CJSON_COUNTING_ALLOC_HISTOGRAM_BUCKETS];
    size_t largest_request;
} cjson_counting_alloc_stats_t;

void cjson_counting_alloc_get_stats(cjson_counting_alloc_stats_t *out);
void cjson_counting_alloc_reset_counters(void);
void cjson_counting_alloc_set_limit(size_t live_bytes_limit);
void cjson_counting_alloc_set_fail_index(uint64_t index);
size_t cjson_counting_alloc_small_max(void);
size_t cjson_counting_alloc_node_size(void);

void *cjson_counting_malloc(size_t size);
void *cjson_counting_calloc(size_t count, size_t size);
void *cjson_counting_realloc(void *pointer, size_t size);
void cjson_counting_free(void *pointer);

#endif
