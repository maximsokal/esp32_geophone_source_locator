#ifndef NRF_THREAD_STACK_INVENTORY_H
#define NRF_THREAD_STACK_INVENTORY_H

#include <stddef.h>
#include <stdint.h>

#define NRF_THREAD_STACK_NAME_CAPACITY 24U

typedef struct {
    const char *name;
    uint32_t stack_size_bytes;
} nrf_thread_stack_inventory_entry_t;

typedef enum {
    NRF_THREAD_STACK_HWM_INVALID = 0,
    NRF_THREAD_STACK_HWM_AVAILABLE,
    NRF_THREAD_STACK_HWM_EXHAUSTED,
} nrf_thread_stack_hwm_t;

extern const nrf_thread_stack_inventory_entry_t nrf_thread_stack_inventory[];
extern const size_t nrf_thread_stack_inventory_count;

nrf_thread_stack_hwm_t nrf_thread_stack_hwm_classify(
    const char *thread_name,
    uint32_t unused_bytes);

#endif
