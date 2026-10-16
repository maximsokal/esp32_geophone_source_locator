#include <assert.h>
#include <string.h>

#include "nrf_thread_stack_inventory.h"

static void test_stack_inventory_is_named_and_sizes_are_bytes(void)
{
    static const uint32_t expected_bytes[] = {
        320U, 3584U, 3072U, 2048U, 2048U, 3200U, 900U, 2100U, 1300U, 1024U, 2048U,
    };

    assert(nrf_thread_stack_inventory_count == sizeof(expected_bytes) / sizeof(expected_bytes[0]));
    for (size_t index = 0U; index < nrf_thread_stack_inventory_count; ++index) {
        assert(nrf_thread_stack_inventory[index].name != NULL);
        assert(nrf_thread_stack_inventory[index].name[0] != '\0');
        assert(strlen(nrf_thread_stack_inventory[index].name) < NRF_THREAD_STACK_NAME_CAPACITY);
        assert(nrf_thread_stack_inventory[index].stack_size_bytes == expected_bytes[index]);
        assert((nrf_thread_stack_inventory[index].stack_size_bytes % 4U) == 0U);
        for (size_t other = index + 1U; other < nrf_thread_stack_inventory_count; ++other) {
            assert(strcmp(
                       nrf_thread_stack_inventory[index].name,
                       nrf_thread_stack_inventory[other].name) != 0);
        }
    }
}

static void test_stack_hwm_classifies_zero_and_enforces_inventory_caps(void)
{
    assert(nrf_thread_stack_hwm_classify("main", 3584U) == NRF_THREAD_STACK_HWM_AVAILABLE);
    assert(nrf_thread_stack_hwm_classify("main", 1U) == NRF_THREAD_STACK_HWM_AVAILABLE);
    assert(nrf_thread_stack_hwm_classify("main", 0U) == NRF_THREAD_STACK_HWM_EXHAUSTED);
    assert(nrf_thread_stack_hwm_classify("main", 3585U) == NRF_THREAD_STACK_HWM_INVALID);
    assert(nrf_thread_stack_hwm_classify("missing", 1U) == NRF_THREAD_STACK_HWM_INVALID);
    assert(nrf_thread_stack_hwm_classify(NULL, 1U) == NRF_THREAD_STACK_HWM_INVALID);
}

int main(void)
{
    test_stack_inventory_is_named_and_sizes_are_bytes();
    test_stack_hwm_classifies_zero_and_enforces_inventory_caps();
    return 0;
}
