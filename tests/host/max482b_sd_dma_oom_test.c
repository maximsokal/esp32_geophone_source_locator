#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

typedef struct {
    uint64_t head;
    size_t size;
    unsigned caps;
    size_t previous_largest_dma_block;
} max482b_allocation_header_t;

static const uint64_t MAX482B_HEAD_GUARD = UINT64_C(0x4D41583438324248);
static const uint64_t MAX482B_TAIL_GUARD = UINT64_C(0x4D41583438324254);
static size_t s_guard_allocation_attempts;
static size_t s_guard_allocations;
static size_t s_guard_frees;
static size_t s_guard_live_allocations;
static size_t s_fail_allocation_nth;
static size_t s_internal_free_bytes;
static size_t s_largest_internal_dma_block;
static size_t s_psram_free_bytes;
static bool s_allocation_failure_triggered;

static unsigned max482b_current_caps(void);

static void *max482b_guard_malloc(size_t size)
{
    ++s_guard_allocation_attempts;
    const unsigned caps = max482b_current_caps();
    bool fail = s_fail_allocation_nth != 0U &&
        s_guard_allocation_attempts == s_fail_allocation_nth;
    if ((caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) ==
            (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) &&
        (size > s_internal_free_bytes || size > s_largest_internal_dma_block)) {
        fail = true;
    }
    if ((caps & MALLOC_CAP_SPIRAM) != 0U && size > s_psram_free_bytes) {
        fail = true;
    }
    if (fail) {
        s_allocation_failure_triggered = true;
        return NULL;
    }

    max482b_allocation_header_t *header = malloc(
        sizeof(*header) + size + sizeof(MAX482B_TAIL_GUARD)
    );
    if (header == NULL) return NULL;
    header->head = MAX482B_HEAD_GUARD;
    header->size = size;
    header->caps = caps;
    header->previous_largest_dma_block = s_largest_internal_dma_block;
    void *payload = header + 1;
    memcpy(
        (unsigned char *)payload + size,
        &MAX482B_TAIL_GUARD,
        sizeof(MAX482B_TAIL_GUARD)
    );
    ++s_guard_allocations;
    ++s_guard_live_allocations;
    if ((caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) ==
        (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) {
        s_internal_free_bytes -= size;
        s_largest_internal_dma_block = s_internal_free_bytes;
    }
    if ((caps & MALLOC_CAP_SPIRAM) != 0U) s_psram_free_bytes -= size;
    return payload;
}

static void max482b_guard_free(void *payload)
{
    if (payload == NULL) return;
    max482b_allocation_header_t *header =
        (max482b_allocation_header_t *)payload - 1;
    uint64_t tail = 0U;
    assert(header->head == MAX482B_HEAD_GUARD);
    memcpy(
        &tail,
        (unsigned char *)payload + header->size,
        sizeof(tail)
    );
    assert(tail == MAX482B_TAIL_GUARD);
    assert(s_guard_live_allocations > 0U);
    if ((header->caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) ==
        (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) {
        s_internal_free_bytes += header->size;
        s_largest_internal_dma_block = header->previous_largest_dma_block;
    }
    if ((header->caps & MALLOC_CAP_SPIRAM) != 0U) s_psram_free_bytes += header->size;
    --s_guard_live_allocations;
    ++s_guard_frees;
    free(header);
}

#define malloc max482b_guard_malloc
#define free max482b_guard_free
#define main max482b_original_sd_card_format_main
#define test_format_success_progress_and_yields sd_card_format_success_test
#include "../../components/sd_card/tests/sd_card_format_host_test.c"
#undef test_format_success_progress_and_yields
#undef main
#undef free
#undef malloc

static unsigned max482b_current_caps(void)
{
    return s_alloc_caps;
}

static void reset_guard_allocator(size_t fail_nth)
{
    assert(s_guard_live_allocations == 0U);
    s_guard_allocation_attempts = 0U;
    s_guard_allocations = 0U;
    s_guard_frees = 0U;
    s_fail_allocation_nth = fail_nth;
    s_internal_free_bytes = 32768U;
    s_largest_internal_dma_block = 2048U;
    s_psram_free_bytes = 1024U * 1024U;
    s_allocation_failure_triggered = false;
}

static void test_psram_can_succeed_when_internal_dma_is_fragmented(void)
{
    reset_fakes();
    reset_guard_allocator(0U);
    void *psram = heap_caps_malloc(64U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(psram != NULL);
    heap_caps_free(psram);
    assert(s_psram_free_bytes == 1024U * 1024U);
    assert(s_guard_live_allocations == 0U);
}

static void test_dma_buffer_oom_fails_without_mount_or_leak(void)
{
    reset_fakes();
    reset_guard_allocator(0U);
    assert(s_internal_free_bytes > 4096U);
    assert(s_largest_internal_dma_block < 4096U);
    assert(s_psram_free_bytes > 4096U);
    assert(sd_card_format_fat32_task_start(&s_card, true) == ESP_OK);
    run_scheduled_task();

    sd_card_format_status_t status;
    assert(sd_card_format_fat32_task_get_status(&status) == ESP_OK);
    assert(s_allocation_failure_triggered);
    assert(s_guard_allocation_attempts == 1U);
    assert(s_guard_allocations == 0U && s_guard_frees == 0U);
    assert(s_guard_live_allocations == 0U);
    assert(status.state == SD_CARD_FORMAT_STATE_FAILED);
    assert(status.err == ESP_ERR_NO_MEM);
    assert(s_alloc_size == 4096U);
    assert(s_alloc_caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    assert(s_mount_calls == 0U && s_probe_calls == 0U);
    assert(!s_fake_mounted && s_task_delete_calls == 1U);
}

static void test_format_success_progress_and_yields(void)
{
    reset_guard_allocator(0U);
    s_largest_internal_dma_block = 8192U;
    sd_card_format_success_test();
    assert(s_guard_allocation_attempts == 1U);
    assert(s_guard_allocations == 1U && s_guard_frees == 1U);
    assert(s_guard_live_allocations == 0U);
}

int main(void)
{
    test_psram_can_succeed_when_internal_dma_is_fragmented();
    test_dma_buffer_oom_fails_without_mount_or_leak();
    test_format_success_progress_and_yields();
    puts("MAX482B_SD_DMA_OOM_PASS");
    return 0;
}
