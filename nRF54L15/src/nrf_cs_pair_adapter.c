#include "nrf_cs_pair_adapter.h"

#include <string.h>

static uint32_t clock_ms(const nrf_cs_pair_adapter_t *adapter)
{
    return adapter->ops.now_ms != NULL ? adapter->ops.now_ms(adapter->ops.context) : 0U;
}

static bool step_matches(const nrf_cs_step_t *a, const nrf_cs_step_t *b)
{
    return a != NULL && b != NULL &&
           a->cycle_id == b->cycle_id &&
           a->membership_generation == b->membership_generation &&
           a->attempt_id == b->attempt_id &&
           a->pair_index == b->pair_index &&
           a->attempt_number == b->attempt_number &&
           a->initiator == b->initiator &&
           a->reflector == b->reflector;
}

static nrf_cs_pair_adapter_result_t validate_event(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    nrf_cs_pair_phase_t expected)
{
    if (adapter == NULL || step == NULL) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    if (!adapter->active || !step_matches(&adapter->step, step)) {
        return NRF_CS_PAIR_ADAPTER_ERR_STALE;
    }
    if (adapter->phase != expected) {
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }
    return NRF_CS_PAIR_ADAPTER_OK;
}

static nrf_cs_pair_adapter_result_t begin_terminal(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_measurement_t *measurement)
{
    if (adapter == NULL || measurement == NULL) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    adapter->terminal_measurement = *measurement;
    adapter->terminal_pending = true;
    adapter->terminal_ms = clock_ms(adapter);
    adapter->phase = NRF_CS_PAIR_PHASE_CLEANUP;
    if (adapter->ops.cleanup(adapter->ops.context) != 0) {
        adapter->terminal_measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
        adapter->terminal_measurement.distance_mm = 0;
        adapter->terminal_measurement.confidence = 0U;
    }
    return NRF_CS_PAIR_ADAPTER_OK;
}

static nrf_cs_pair_adapter_result_t fail_transport(nrf_cs_pair_adapter_t *adapter)
{
    nrf_cs_measurement_t measurement;
    memset(&measurement, 0, sizeof(measurement));
    measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
    return begin_terminal(adapter, &measurement);
}

static nrf_cs_pair_adapter_result_t advance(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    nrf_cs_pair_phase_t expected,
    nrf_cs_pair_phase_t next,
    int (*operation)(void *))
{
    nrf_cs_pair_adapter_result_t result = validate_event(adapter, step, expected);
    if (result != NRF_CS_PAIR_ADAPTER_OK) {
        /* `operation` may be NULL here: the wrapper could not read it from a NULL adapter. */
        return result;
    }
    adapter->phase = next;
    if (operation(adapter->ops.context) != 0) {
        return fail_transport(adapter);
    }
    return NRF_CS_PAIR_ADAPTER_OK;
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_init(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_pair_adapter_ops_t *ops)
{
    if (adapter == NULL || ops == NULL ||
        ops->connect == NULL || ops->secure == NULL ||
        ops->exchange_mtu == NULL || ops->discover_ras == NULL ||
        ops->read_features == NULL || ops->configure_ranging_data == NULL ||
        ops->exchange_capabilities == NULL || ops->create_config == NULL ||
        ops->enable_cs_security == NULL || ops->start_procedure == NULL ||
        ops->cleanup == NULL) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    memset(adapter, 0, sizeof(*adapter));
    adapter->ops = *ops;
    adapter->phase = NRF_CS_PAIR_PHASE_IDLE;
    return NRF_CS_PAIR_ADAPTER_OK;
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_execute(
    nrf_cs_pair_adapter_t *adapter,
    nrf_cs_node_id_t local_node_id,
    const nrf_cs_step_t *step,
    nrf_cs_pair_adapter_complete_cb_t complete_cb,
    void *complete_context)
{
    if (adapter == NULL || step == NULL || complete_cb == NULL ||
        step->timeout_ms == 0U || step->initiator == step->reflector ||
        local_node_id != step->initiator) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    if (adapter->active) {
        return NRF_CS_PAIR_ADAPTER_ERR_BUSY;
    }

    adapter->active = true;
    adapter->phase = NRF_CS_PAIR_PHASE_CONNECTING;
    adapter->step = *step;
    adapter->complete_cb = complete_cb;
    adapter->complete_context = complete_context;
    adapter->terminal_pending = false;
    adapter->timeout_during_cleanup = false;
    adapter->started_ms = clock_ms(adapter);
    memset(&adapter->terminal_measurement, 0, sizeof(adapter->terminal_measurement));

    if (adapter->ops.connect(adapter->ops.context, step->reflector) != 0) {
        return fail_transport(adapter);
    }
    return NRF_CS_PAIR_ADAPTER_OK;
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_connected(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_CONNECTING,
                   NRF_CS_PAIR_PHASE_SECURING, adapter != NULL ? adapter->ops.secure : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_security_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_SECURING,
                   NRF_CS_PAIR_PHASE_MTU, adapter != NULL ? adapter->ops.exchange_mtu : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_mtu_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_MTU,
                   NRF_CS_PAIR_PHASE_DISCOVERING, adapter != NULL ? adapter->ops.discover_ras : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_discovery_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_DISCOVERING,
                   NRF_CS_PAIR_PHASE_FEATURES, adapter != NULL ? adapter->ops.read_features : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_features_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    bool realtime)
{
    nrf_cs_pair_adapter_result_t result =
        validate_event(adapter, step, NRF_CS_PAIR_PHASE_FEATURES);
    if (result != NRF_CS_PAIR_ADAPTER_OK) {
        return result;
    }
    if (adapter->ops.configure_ranging_data(adapter->ops.context, realtime) != 0) {
        return fail_transport(adapter);
    }
    adapter->phase = NRF_CS_PAIR_PHASE_CAPABILITIES;
    if (adapter->ops.exchange_capabilities(adapter->ops.context) != 0) {
        return fail_transport(adapter);
    }
    return NRF_CS_PAIR_ADAPTER_OK;
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_capabilities_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_CAPABILITIES,
                   NRF_CS_PAIR_PHASE_CONFIG, adapter != NULL ? adapter->ops.create_config : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_config_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_CONFIG,
                   NRF_CS_PAIR_PHASE_CS_SECURITY, adapter != NULL ? adapter->ops.enable_cs_security : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_cs_security_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    return advance(adapter, step, NRF_CS_PAIR_PHASE_CS_SECURITY,
                   NRF_CS_PAIR_PHASE_PROCEDURE, adapter != NULL ? adapter->ops.start_procedure : NULL);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_measurement_ready(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step,
    const nrf_cs_measurement_t *measurement)
{
    nrf_cs_pair_adapter_result_t result =
        validate_event(adapter, step, NRF_CS_PAIR_PHASE_PROCEDURE);
    if (result != NRF_CS_PAIR_ADAPTER_OK || measurement == NULL) {
        return measurement == NULL ? NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT : result;
    }
    return begin_terminal(adapter, measurement);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_transport_error(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    if (adapter == NULL || step == NULL) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    if (!adapter->active || !step_matches(&adapter->step, step)) {
        return NRF_CS_PAIR_ADAPTER_ERR_STALE;
    }
    if (adapter->phase == NRF_CS_PAIR_PHASE_CLEANUP ||
        adapter->phase == NRF_CS_PAIR_PHASE_IDLE) {
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }
    return fail_transport(adapter);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_timeout(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    nrf_cs_measurement_t measurement;
    if (adapter == NULL || step == NULL) {
        return NRF_CS_PAIR_ADAPTER_ERR_ARGUMENT;
    }
    if (!adapter->active || !step_matches(&adapter->step, step)) {
        return NRF_CS_PAIR_ADAPTER_ERR_STALE;
    }
    if (adapter->phase == NRF_CS_PAIR_PHASE_CLEANUP) {
        /* The outcome is already known; only the disconnect is late. Keep the outcome, remember the lateness. */
        adapter->timeout_during_cleanup = true;
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }
    if (adapter->phase == NRF_CS_PAIR_PHASE_IDLE) {
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }
    memset(&measurement, 0, sizeof(measurement));
    measurement.outcome = NRF_CS_OUTCOME_TIMEOUT;
    return begin_terminal(adapter, &measurement);
}

nrf_cs_pair_adapter_result_t nrf_cs_pair_adapter_cleanup_complete(
    nrf_cs_pair_adapter_t *adapter,
    const nrf_cs_step_t *step)
{
    nrf_cs_step_t completed_step;
    nrf_cs_measurement_t completed_measurement;
    nrf_cs_pair_adapter_complete_cb_t callback;
    void *callback_context;
    nrf_cs_pair_adapter_result_t result =
        validate_event(adapter, step, NRF_CS_PAIR_PHASE_CLEANUP);

    if (result != NRF_CS_PAIR_ADAPTER_OK) {
        return result;
    }
    if (!adapter->terminal_pending) {
        return NRF_CS_PAIR_ADAPTER_ERR_STATE;
    }

    memset(&adapter->last_timing, 0, sizeof(adapter->last_timing));
    if (adapter->ops.now_ms != NULL) {
        const uint32_t now = clock_ms(adapter);
        adapter->last_timing.valid = true;
        adapter->last_timing.measured_ms = adapter->terminal_ms - adapter->started_ms;
        adapter->last_timing.cleanup_ms = now - adapter->terminal_ms;
        adapter->last_timing.total_ms = now - adapter->started_ms;
        adapter->last_timing.budget_exceeded = adapter->last_timing.total_ms > adapter->step.timeout_ms;
        adapter->last_timing.timeout_during_cleanup = adapter->timeout_during_cleanup;
    }
    completed_step = adapter->step;
    completed_measurement = adapter->terminal_measurement;
    callback = adapter->complete_cb;
    callback_context = adapter->complete_context;

    adapter->active = false;
    adapter->phase = NRF_CS_PAIR_PHASE_IDLE;
    adapter->terminal_pending = false;
    adapter->complete_cb = NULL;
    adapter->complete_context = NULL;

    callback(&completed_step, &completed_measurement, callback_context);
    return NRF_CS_PAIR_ADAPTER_COMPLETED;
}

bool nrf_cs_pair_adapter_last_timing(const nrf_cs_pair_adapter_t *adapter, nrf_cs_pair_adapter_timing_t *out)
{
    if (adapter == NULL || out == NULL) return false;
    *out = adapter->last_timing;
    return adapter->last_timing.valid;
}

bool nrf_cs_pair_adapter_is_active(const nrf_cs_pair_adapter_t *adapter)
{
    return adapter != NULL && adapter->active;
}

nrf_cs_pair_phase_t nrf_cs_pair_adapter_phase(const nrf_cs_pair_adapter_t *adapter)
{
    return adapter == NULL ? NRF_CS_PAIR_PHASE_IDLE : adapter->phase;
}

const nrf_cs_step_t *nrf_cs_pair_adapter_active_step(const nrf_cs_pair_adapter_t *adapter)
{
    if (adapter == NULL || !adapter->active) {
        return NULL;
    }
    return &adapter->step;
}
