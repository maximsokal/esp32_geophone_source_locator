#include "nrf_cs_pair_runtime_zephyr.h"

int nrf_cs_pair_runtime_zephyr_start(void)
{
    return 0;
}

int nrf_cs_pair_runtime_zephyr_set_node_id(nrf_cs_node_id_t node_id)
{
    (void)node_id;
    return 0;
}

int nrf_cs_pair_runtime_zephyr_execute(
    const nrf_cs_step_t *step,
    nrf_cs_pair_runtime_complete_cb_t complete_cb,
    void *context)
{
    (void)step;
    (void)complete_cb;
    (void)context;
    return 0;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return false;
}
