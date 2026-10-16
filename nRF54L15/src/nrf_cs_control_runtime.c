#include "nrf_cs_control_runtime.h"
#include <string.h>

static void make_running(const nrf_cs_control_request_t *request, nrf_cs_control_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->request_id = request->request_id;
    out->step = request->step;
    out->state = NRF_CS_CONTROL_STATE_RUNNING;
    out->command_result = NRF_CS_CONTROL_RESULT_PENDING;
    out->measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
}

static void make_rejected(const nrf_cs_control_request_t *request, uint8_t command_result, nrf_cs_control_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->request_id = request->request_id;
    out->step = request->step;
    out->state = NRF_CS_CONTROL_STATE_FAILED;
    out->command_result = command_result;
    out->measurement.outcome = NRF_CS_OUTCOME_TRANSPORT_ERROR;
}

static bool same_request(const nrf_cs_control_request_t *a, const nrf_cs_control_request_t *b)
{
    return a->request_id == b->request_id && nrf_cs_control_step_equal(&a->step, &b->step);
}

static bool result_matches(const nrf_cs_control_request_t *request, const nrf_cs_control_result_t *result)
{
    return request->request_id == result->request_id && nrf_cs_control_step_equal(&request->step, &result->step);
}

int nrf_cs_control_runtime_init(nrf_cs_control_runtime_t *runtime)
{
    if (runtime == NULL) return -1;
    memset(runtime, 0, sizeof(*runtime));
    runtime->local_node_id = 0xFFU;
    return 0;
}

int nrf_cs_control_runtime_assign_node(nrf_cs_control_runtime_t *runtime, nrf_cs_node_id_t node_id)
{
    if (runtime == NULL || node_id == 0xFFU) return -1;
    if (runtime->active && runtime->local_node_id != node_id) return -2;
    runtime->local_node_id = node_id;
    runtime->assigned = true;
    return 0;
}

int nrf_cs_control_runtime_accept_execute(nrf_cs_control_runtime_t *runtime, const nrf_cs_control_request_t *request, nrf_cs_control_result_t *status_out, bool *start_required)
{
    if (runtime == NULL || request == NULL || status_out == NULL || start_required == NULL || nrf_cs_control_validate_request(request) != 0) return -1;
    *start_required = false;

    if (!runtime->assigned || request->step.initiator != runtime->local_node_id) {
        make_rejected(request, NRF_CS_CONTROL_RESULT_INVALID, status_out);
        runtime->immediate_result = *status_out;
        runtime->immediate_valid = true;
        return 0;
    }
    if (runtime->active) {
        if (same_request(request, &runtime->active_request)) {
            make_running(request, status_out);
            return 0;
        }
        make_rejected(request, NRF_CS_CONTROL_RESULT_BUSY, status_out);
        runtime->immediate_result = *status_out;
        runtime->immediate_valid = true;
        return 0;
    }
    if (runtime->terminal_valid && result_matches(request, &runtime->terminal_result)) {
        *status_out = runtime->terminal_result;
        return 0;
    }

    runtime->immediate_valid = false;
    runtime->active = true;
    runtime->active_request = *request;
    make_running(request, status_out);
    *start_required = true;
    return 0;
}

int nrf_cs_control_runtime_record_enqueue_result(
    nrf_cs_control_runtime_t *runtime,
    const nrf_cs_control_request_t *request,
    uint8_t command_result)
{
    if (runtime == NULL || request == NULL ||
        nrf_cs_control_validate_request(request) != 0 ||
        (command_result != NRF_CS_CONTROL_RESULT_BUSY &&
         command_result != NRF_CS_CONTROL_RESULT_INTERNAL_ERROR)) {
        return -1;
    }
    make_rejected(request, command_result, &runtime->enqueue_result);
    runtime->enqueue_result_valid = true;
    return 0;
}

int nrf_cs_control_runtime_complete(nrf_cs_control_runtime_t *runtime, const nrf_cs_step_t *step, const nrf_cs_measurement_t *measurement)
{
    if (runtime == NULL || step == NULL || measurement == NULL) return -1;
    if (!runtime->active || !nrf_cs_control_step_equal(step, &runtime->active_request.step)) return -2;
    memset(&runtime->terminal_result, 0, sizeof(runtime->terminal_result));
    runtime->terminal_result.request_id = runtime->active_request.request_id;
    runtime->terminal_result.step = runtime->active_request.step;
    runtime->terminal_result.measurement = *measurement;
    runtime->terminal_result.state = measurement->outcome == NRF_CS_OUTCOME_SUCCESS ? NRF_CS_CONTROL_STATE_COMPLETE : NRF_CS_CONTROL_STATE_FAILED;
    runtime->terminal_result.command_result = NRF_CS_CONTROL_RESULT_OK;
    runtime->terminal_valid = true;
    runtime->active = false;
    memset(&runtime->active_request, 0, sizeof(runtime->active_request));
    return 0;
}

int nrf_cs_control_runtime_select_result(nrf_cs_control_runtime_t *runtime, const nrf_cs_control_request_t *request)
{
    if (runtime == NULL || request == NULL || nrf_cs_control_validate_request(request) != 0) return -1;
    runtime->selector = *request;
    runtime->selector_armed = true;
    if (runtime->active && same_request(request, &runtime->active_request)) { runtime->selector_stale = false; return 0; }
    if (runtime->terminal_valid && result_matches(request, &runtime->terminal_result)) { runtime->selector_stale = false; return 0; }
    if (runtime->immediate_valid && result_matches(request, &runtime->immediate_result)) { runtime->selector_stale = false; return 0; }
    if (runtime->enqueue_result_valid && result_matches(request, &runtime->enqueue_result)) { runtime->selector_stale = false; return 0; }
    runtime->selector_stale = true;
    return -2;
}

bool nrf_cs_control_runtime_take_selected(nrf_cs_control_runtime_t *runtime, nrf_cs_control_result_t *result_out)
{
    if (runtime == NULL || result_out == NULL || !runtime->selector_armed) return false;
    runtime->selector_armed = false;
    if (runtime->selector_stale) {
        make_rejected(&runtime->selector, NRF_CS_CONTROL_RESULT_STALE, result_out);
        runtime->selector_stale = false;
        return true;
    }
    if (runtime->active && same_request(&runtime->selector, &runtime->active_request)) {
        make_running(&runtime->active_request, result_out);
        return true;
    }
    if (runtime->terminal_valid && result_matches(&runtime->selector, &runtime->terminal_result)) {
        *result_out = runtime->terminal_result;
        return true;
    }
    if (runtime->immediate_valid && result_matches(&runtime->selector, &runtime->immediate_result)) {
        *result_out = runtime->immediate_result;
        runtime->immediate_valid = false;
        return true;
    }
    if (runtime->enqueue_result_valid && result_matches(&runtime->selector, &runtime->enqueue_result)) {
        *result_out = runtime->enqueue_result;
        runtime->enqueue_result_valid = false;
        return true;
    }
    return false;
}
