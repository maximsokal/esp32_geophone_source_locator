#include "nrf_cs_pair_runtime_zephyr.h"

static int s_next_execute_result;
static nrf_cs_outcome_t s_next_outcome;
unsigned nrf_cs_pair_zephyr_stub_execute_calls;

void nrf_cs_pair_runtime_zephyr_stub_configure(int execute_result, nrf_cs_outcome_t outcome)
{
    s_next_execute_result = execute_result;
    s_next_outcome = outcome;
}

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
    nrf_cs_measurement_t measurement = {
        .outcome = s_next_outcome,
    };
    int execute_result = s_next_execute_result;
    ++nrf_cs_pair_zephyr_stub_execute_calls;
    s_next_execute_result = 0;
    s_next_outcome = NRF_CS_OUTCOME_SUCCESS;
    if (execute_result != 0) {
        return execute_result;
    }
    if (complete_cb != NULL) {
        complete_cb(step, &measurement, context);
    }
    return 0;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return false;
}
