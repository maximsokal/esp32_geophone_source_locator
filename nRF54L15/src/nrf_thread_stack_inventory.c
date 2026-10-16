#include "nrf_thread_stack_inventory.h"

#include <string.h>

/* UNVERIFIED-ON-TARGET: existing entries reflect the 2026-10-10 NCS build snapshot. */
const nrf_thread_stack_inventory_entry_t nrf_thread_stack_inventory[] = {
    {"idle", 320U},
    {"main", 3584U},
    {"sensor_worker", 3072U},
    {"system_workqueue", 2048U},
    {"cs_hci_workqueue", 2048U},
    {"bt_rx", 3200U},
    {"bt_tx", 900U},
    {"bt_long_workqueue", 2100U},
    {"gatt_discovery", 1300U},
    {"ras_rrsp", 1024U},
    {"isr", 2048U},
};

const size_t nrf_thread_stack_inventory_count =
    sizeof(nrf_thread_stack_inventory) / sizeof(nrf_thread_stack_inventory[0]);

nrf_thread_stack_hwm_t nrf_thread_stack_hwm_classify(
    const char *thread_name,
    uint32_t unused_bytes)
{
    if (thread_name == NULL) {
        return NRF_THREAD_STACK_HWM_INVALID;
    }
    for (size_t index = 0U; index < nrf_thread_stack_inventory_count; ++index) {
        const nrf_thread_stack_inventory_entry_t *entry = &nrf_thread_stack_inventory[index];

        if (strcmp(entry->name, thread_name) != 0) {
            continue;
        }
        if (entry->stack_size_bytes == 0U || unused_bytes > entry->stack_size_bytes) {
            return NRF_THREAD_STACK_HWM_INVALID;
        }
        return unused_bytes == 0U
            ? NRF_THREAD_STACK_HWM_EXHAUSTED
            : NRF_THREAD_STACK_HWM_AVAILABLE;
    }
    return NRF_THREAD_STACK_HWM_INVALID;
}
