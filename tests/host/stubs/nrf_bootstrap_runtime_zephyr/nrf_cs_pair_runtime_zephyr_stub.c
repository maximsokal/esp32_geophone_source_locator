#include "nrf_cs_pair_runtime_zephyr.h"

static int s_next_execute_result;
static nrf_cs_outcome_t s_next_outcome;
unsigned nrf_cs_pair_zephyr_stub_execute_calls;
int nrf_cs_pair_zephyr_stub_set_node_result;
bool nrf_cs_pair_zephyr_stub_hold;
unsigned nrf_cs_pair_zephyr_stub_cancel_calls;
nrf_cs_step_t nrf_cs_pair_zephyr_stub_cancel_step;
static nrf_cs_step_t s_held_step;
static nrf_cs_pair_runtime_complete_cb_t s_held_cb;
static void *s_held_context;

void nrf_cs_pair_runtime_zephyr_cancel(const nrf_cs_step_t *step)
{
    ++nrf_cs_pair_zephyr_stub_cancel_calls;
    nrf_cs_pair_zephyr_stub_cancel_step=*step;
}

void nrf_cs_pair_zephyr_stub_finish_held(void)
{
    const nrf_cs_measurement_t m={.outcome=NRF_CS_OUTCOME_SUCCESS};
    nrf_cs_pair_runtime_complete_cb_t cb=s_held_cb;s_held_cb=NULL;
    if(cb!=NULL)cb(&s_held_step,&m,s_held_context);
}

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
    return nrf_cs_pair_zephyr_stub_set_node_result;
}

nrf_cs_pair_runtime_final_hint_cb_t nrf_cs_pair_zephyr_stub_final_hint;
nrf_cs_pair_runtime_released_cb_t nrf_cs_pair_zephyr_stub_released;

void nrf_cs_pair_runtime_zephyr_set_relay_hooks(
    nrf_cs_pair_runtime_final_hint_cb_t final_hint, nrf_cs_pair_runtime_released_cb_t released)
{
    nrf_cs_pair_zephyr_stub_final_hint = final_hint;
    nrf_cs_pair_zephyr_stub_released = released;
}

bool nrf_cs_pair_zephyr_stub_radio_enabled;
unsigned nrf_cs_pair_zephyr_stub_radio_changes;

int nrf_cs_pair_runtime_zephyr_set_radio_enabled(bool enabled)
{
    nrf_cs_pair_zephyr_stub_radio_enabled = enabled;
    ++nrf_cs_pair_zephyr_stub_radio_changes;
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
    if (nrf_cs_pair_zephyr_stub_hold) {
        s_held_step=*step;s_held_cb=complete_cb;s_held_context=context;
    } else if (complete_cb != NULL) {
        complete_cb(step, &measurement, context);
    }
    return 0;
}

bool nrf_cs_pair_runtime_zephyr_is_busy(void)
{
    return false;
}
