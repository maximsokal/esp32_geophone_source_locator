#ifndef NRF_THREAD_STACK_INVENTORY_H
#define NRF_THREAD_STACK_INVENTORY_H

#include <stddef.h>
#include <stdint.h>

#define NRF_THREAD_STACK_NAME_CAPACITY 24U

typedef struct {
    const char *name;
    uint32_t stack_size_bytes;
} nrf_thread_stack_inventory_entry_t;

extern const nrf_thread_stack_inventory_entry_t nrf_thread_stack_inventory[];
extern const size_t nrf_thread_stack_inventory_count;

#endif
