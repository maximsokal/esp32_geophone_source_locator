#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "cjson_alloc_guard.h"

static int s_failing_calls;
static int s_counting_calls;

static void *failing_allocator(size_t size)
{
    (void)size;
    ++s_failing_calls;
    return NULL;
}

static void *counting_allocator(size_t size)
{
    ++s_counting_calls;
    return malloc(size);
}

static void test_default_allocator_succeeds_without_counting(void)
{
    cjson_alloc_guard_set_allocator(NULL);
    const uint32_t before = cjson_alloc_guard_failures();

    void *block = cjson_alloc_guard_alloc(32U);
    assert(block != NULL);
    free(block);

    assert(cjson_alloc_guard_failures() == before);
}

static void test_failed_allocation_is_counted_once_per_failure(void)
{
    const uint32_t before = cjson_alloc_guard_failures();

    cjson_alloc_guard_set_allocator(failing_allocator);
    assert(cjson_alloc_guard_alloc(16U) == NULL);
    assert(cjson_alloc_guard_failures() == before + 1U);
    assert(cjson_alloc_guard_alloc(64U) == NULL);
    assert(cjson_alloc_guard_failures() == before + 2U);
    assert(s_failing_calls == 2);

    /* A zero-sized request may legally return NULL and is not a failure. */
    assert(cjson_alloc_guard_alloc(0U) == NULL);
    assert(cjson_alloc_guard_failures() == before + 2U);

    cjson_alloc_guard_set_allocator(NULL);
}

static void test_allocator_can_be_swapped_and_restored(void)
{
    const uint32_t before = cjson_alloc_guard_failures();

    cjson_alloc_guard_set_allocator(counting_allocator);
    void *block = cjson_alloc_guard_alloc(8U);
    assert(block != NULL);
    free(block);
    assert(s_counting_calls == 1);

    cjson_alloc_guard_set_allocator(NULL);
    block = cjson_alloc_guard_alloc(8U);
    assert(block != NULL);
    free(block);
    assert(s_counting_calls == 1);
    assert(cjson_alloc_guard_failures() == before);
}

static void test_external_failure_note_is_counted(void)
{
    const uint32_t before = cjson_alloc_guard_failures();
    cjson_alloc_guard_note_failure();
    assert(cjson_alloc_guard_failures() == before + 1U);
}

static void test_install_is_idempotent_on_host(void)
{
    cjson_alloc_guard_install();
    cjson_alloc_guard_install();
}

int main(void)
{
    test_default_allocator_succeeds_without_counting();
    test_failed_allocation_is_counted_once_per_failure();
    test_allocator_can_be_swapped_and_restored();
    test_external_failure_note_is_counted();
    test_install_is_idempotent_on_host();
    return 0;
}
