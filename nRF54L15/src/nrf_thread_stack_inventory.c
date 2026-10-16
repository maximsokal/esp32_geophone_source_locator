#include "nrf_thread_stack_inventory.h"

/* UNVERIFIED-ON-TARGET: this byte inventory reflects the 2026-10-10 NCS build snapshot. */
const nrf_thread_stack_inventory_entry_t nrf_thread_stack_inventory[] = {
    {"idle", 320U},
    {"main", 3584U},
    {"sensor_worker", 3072U},
    {"system_workqueue", 2048U},
    {"bt_rx", 3200U},
    {"bt_tx", 900U},
    {"bt_long_workqueue", 2100U},
    {"gatt_discovery", 1300U},
    {"ras_rrsp", 1024U},
    {"isr", 2048U},
};

const size_t nrf_thread_stack_inventory_count =
    sizeof(nrf_thread_stack_inventory) / sizeof(nrf_thread_stack_inventory[0]);
