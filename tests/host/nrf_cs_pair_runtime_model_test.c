#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_control_runtime.h"
#include "nrf_cs_pair_adapter.h"
#include "nrf_cs_pair_runtime_zephyr_bridge.h"

typedef struct {
    unsigned connects;
    unsigned transitions;
    unsigned cleanups;
    unsigned completions;
    nrf_cs_outcome_t outcome;
    nrf_cs_control_runtime_t control;
    nrf_cs_pair_adapter_t adapter;
} runtime_model_t;

static int op_connect(void *context, nrf_cs_node_id_t reflector)
{
    runtime_model_t *model = context;
    assert(reflector == 3U);
    ++model->connects;
    return 0;
}

static int op_transition(void *context)
{
    runtime_model_t *model = context;
    ++model->transitions;
    return 0;
}

static int op_configure(void *context, bool realtime)
{
    runtime_model_t *model = context;
    (void)realtime;
    ++model->transitions;
    return 0;
}

static int op_cleanup(void *context)
{
    runtime_model_t *model = context;
    ++model->cleanups;
    return 0;
}

static void completed(const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement, void *context)
{
    runtime_model_t *model = context;
    assert(nrf_cs_control_runtime_complete(&model->control, step, measurement) == 0);
    model->outcome = measurement->outcome;
    ++model->completions;
}

static nrf_cs_pair_adapter_ops_t adapter_ops(runtime_model_t *model)
{
    return (nrf_cs_pair_adapter_ops_t){
        .connect = op_connect, .secure = op_transition, .exchange_mtu = op_transition,
        .discover_ras = op_transition, .read_features = op_transition,
        .configure_ranging_data = op_configure, .exchange_capabilities = op_transition,
        .create_config = op_transition, .enable_cs_security = op_transition,
        .start_procedure = op_transition, .cleanup = op_cleanup, .context = model,
    };
}

static nrf_cs_pair_adapter_result_t deliver_bridge_event(
    runtime_model_t *model,
    nrf_cs_pair_runtime_bridge_event_t event,
    const nrf_cs_step_t *step,
    bool realtime,
    const nrf_cs_measurement_t *measurement)
{
    if (!nrf_cs_pair_runtime_bridge_step_matches(&model->adapter, step)) {
        return NRF_CS_PAIR_ADAPTER_ERR_STALE;
    }
    return nrf_cs_pair_runtime_bridge_dispatch(
        &model->adapter, event, step, realtime, measurement);
}

static nrf_cs_step_t make_step(uint32_t attempt, uint32_t membership_generation)
{
    return (nrf_cs_step_t){
        .cycle_id = 1U, .membership_generation = membership_generation, .attempt_id = attempt,
        .pair_index = 0U, .attempt_number = 1U, .initiator = 2U, .reflector = 3U,
        .timeout_ms = 500U,
    };
}

static nrf_cs_control_request_t make_request(const nrf_cs_step_t *step, uint32_t request_id)
{
    return (nrf_cs_control_request_t){.request_id = request_id, .step = *step};
}

static void start_procedure(runtime_model_t *model, const nrf_cs_step_t *step, uint32_t request_id)
{
    nrf_cs_control_result_t status;
    nrf_cs_control_request_t request = make_request(step, request_id);
    bool start_required = false;
    assert(nrf_cs_control_runtime_accept_execute(&model->control, &request, &status, &start_required) == 0);
    assert(start_required && status.command_result == NRF_CS_CONTROL_RESULT_PENDING);
    assert(nrf_cs_pair_adapter_execute(&model->adapter, 2U, step, completed, model) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_CONNECTED, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_SECURITY_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_MTU_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_DISCOVERY_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_FEATURES_READY, step, true, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_CAPABILITIES_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_CONFIG_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(model, NRF_CS_PAIR_BRIDGE_CS_SECURITY_READY, step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&model->adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);
}

static nrf_cs_control_result_t take_result(runtime_model_t *model, const nrf_cs_control_request_t *request)
{
    nrf_cs_control_result_t result;
    assert(nrf_cs_control_runtime_select_result(&model->control, request) == 0);
    assert(nrf_cs_control_runtime_take_selected(&model->control, &result));
    return result;
}

/* mutation-only: stale membership, duplicate finish, and late events cannot replace a terminal result. */
static void test_duplicate_late_partial_and_stale_step(void)
{
    runtime_model_t model = {0};
    const nrf_cs_pair_adapter_ops_t ops = adapter_ops(&model);
    const nrf_cs_step_t step = make_step(10U, 4U);
    nrf_cs_step_t stale = step;
    nrf_cs_measurement_t measurement = {.outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 2222, .confidence = 900U};
    bool local_complete = false;
    bool peer_complete = false;
    assert(nrf_cs_control_runtime_init(&model.control) == 0);
    assert(nrf_cs_control_runtime_assign_node(&model.control, 2U) == 0);
    assert(nrf_cs_pair_adapter_init(&model.adapter, &ops) == NRF_CS_PAIR_ADAPTER_OK);
    start_procedure(&model, &step, 100U);

    stale.membership_generation++;
    assert(!nrf_cs_pair_runtime_bridge_step_matches(&model.adapter, &stale));
    assert(nrf_cs_pair_runtime_bridge_dispatch(
        &model.adapter, NRF_CS_PAIR_BRIDGE_CONNECTED, &stale, false, NULL) ==
        NRF_CS_PAIR_ADAPTER_ERR_STALE);
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_CONNECTED,
        &stale, false, NULL) == NRF_CS_PAIR_ADAPTER_ERR_STALE);
    assert(nrf_cs_pair_adapter_phase(&model.adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);

    assert(!nrf_cs_pair_runtime_bridge_record_completion(&local_complete, &peer_complete, true));
    assert(nrf_cs_pair_adapter_phase(&model.adapter) == NRF_CS_PAIR_PHASE_PROCEDURE);
    assert(model.completions == 0U);
    assert(nrf_cs_pair_runtime_bridge_record_completion(&local_complete, &peer_complete, false));
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY,
        &step, false, &measurement) == NRF_CS_PAIR_ADAPTER_OK);
    assert(nrf_cs_pair_adapter_phase(&model.adapter) == NRF_CS_PAIR_PHASE_CLEANUP);
    assert(model.adapter.terminal_pending);
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY,
        &step, false, &measurement) == NRF_CS_PAIR_ADAPTER_ERR_STATE);
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE,
        &step, false, NULL) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(model.completions == 1U && model.outcome == NRF_CS_OUTCOME_SUCCESS);
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE,
        &step, false, NULL) == NRF_CS_PAIR_ADAPTER_ERR_STALE);
    assert(deliver_bridge_event(&model, NRF_CS_PAIR_BRIDGE_MEASUREMENT_READY,
        &step, false, &measurement) == NRF_CS_PAIR_ADAPTER_ERR_STALE);

    const nrf_cs_control_result_t result = take_result(&model, &(nrf_cs_control_request_t){.request_id=100U,.step=step});
    assert(result.state == NRF_CS_CONTROL_STATE_COMPLETE && result.command_result == NRF_CS_CONTROL_RESULT_OK);
    assert(result.measurement.distance_mm == 2222);
}

/* characterization: timeout after partial progress and a link drop both reach one terminal control result. */
static void test_timeout_and_link_drop(void)
{
    runtime_model_t timeout_model = {0};
    const nrf_cs_pair_adapter_ops_t timeout_ops = adapter_ops(&timeout_model);
    const nrf_cs_step_t timeout_step = make_step(11U, 5U);
    assert(nrf_cs_control_runtime_init(&timeout_model.control) == 0);
    assert(nrf_cs_control_runtime_assign_node(&timeout_model.control, 2U) == 0);
    assert(nrf_cs_pair_adapter_init(&timeout_model.adapter, &timeout_ops) == NRF_CS_PAIR_ADAPTER_OK);
    start_procedure(&timeout_model, &timeout_step, 101U);
    assert(deliver_bridge_event(&timeout_model, NRF_CS_PAIR_BRIDGE_TIMEOUT,
        &timeout_step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(timeout_model.completions == 0U);
    assert(deliver_bridge_event(&timeout_model, NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE,
        &timeout_step, false, NULL) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(timeout_model.completions == 1U && timeout_model.outcome == NRF_CS_OUTCOME_TIMEOUT);
    nrf_cs_control_result_t timeout_result = take_result(&timeout_model,
        &(nrf_cs_control_request_t){.request_id=101U,.step=timeout_step});
    assert(timeout_result.state == NRF_CS_CONTROL_STATE_FAILED);
    assert(timeout_result.measurement.outcome == NRF_CS_OUTCOME_TIMEOUT);

    runtime_model_t link_model = {0};
    const nrf_cs_pair_adapter_ops_t link_ops = adapter_ops(&link_model);
    const nrf_cs_step_t link_step = make_step(12U, 6U);
    assert(nrf_cs_control_runtime_init(&link_model.control) == 0);
    assert(nrf_cs_control_runtime_assign_node(&link_model.control, 2U) == 0);
    assert(nrf_cs_pair_adapter_init(&link_model.adapter, &link_ops) == NRF_CS_PAIR_ADAPTER_OK);
    start_procedure(&link_model, &link_step, 102U);
    assert(deliver_bridge_event(&link_model, NRF_CS_PAIR_BRIDGE_DISCONNECTED,
        &link_step, false, NULL) == NRF_CS_PAIR_ADAPTER_OK);
    assert(deliver_bridge_event(&link_model, NRF_CS_PAIR_BRIDGE_CLEANUP_COMPLETE,
        &link_step, false, NULL) == NRF_CS_PAIR_ADAPTER_COMPLETED);
    assert(link_model.completions == 1U && link_model.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
    nrf_cs_control_result_t link_result = take_result(&link_model,
        &(nrf_cs_control_request_t){.request_id=102U,.step=link_step});
    assert(link_result.state == NRF_CS_CONTROL_STATE_FAILED);
    assert(link_result.measurement.outcome == NRF_CS_OUTCOME_TRANSPORT_ERROR);
}

/* characterization: entry point for the production pair adapter and control runtime model. */
int main(void)
{
    test_duplicate_late_partial_and_stale_step();
    test_timeout_and_link_drop();
    return 0;
}
