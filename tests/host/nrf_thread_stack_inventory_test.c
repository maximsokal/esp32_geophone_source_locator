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
    }
}

int main(void)
{
    test_stack_inventory_is_named_and_sizes_are_bytes();
    return 0;
}
