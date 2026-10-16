#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_control_runtime.h"

static nrf_cs_control_request_t make_request(uint32_t request_id, uint32_t attempt_id, uint8_t initiator)
{
    nrf_cs_control_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = request_id;
    request.step.cycle_id = 1U;
    request.step.membership_generation = 1U;
    request.step.attempt_id = attempt_id;
    request.step.pair_index = 0U;
    request.step.attempt_number = 1U;
    request.step.initiator = initiator;
    request.step.reflector = 3U;
    request.step.timeout_ms = 500U;
    return request;
}

int main(void)
{
    nrf_cs_control_runtime_t runtime;
    nrf_cs_control_request_t request = make_request(10U, 1U, 2U);
    nrf_cs_control_request_t other = make_request(11U, 2U, 2U);
    nrf_cs_control_request_t invalid = make_request(12U, 3U, 4U);
    nrf_cs_control_request_t stale = make_request(13U, 4U, 2U);
    nrf_cs_control_result_t status;
    nrf_cs_control_result_t selected;
    nrf_cs_measurement_t measurement;
    bool start_required = false;

    assert(nrf_cs_control_runtime_init(&runtime) == 0);
    assert(nrf_cs_control_runtime_assign_node(&runtime, 2U) == 0);

    assert(nrf_cs_control_runtime_accept_execute(&runtime, &request, &status, &start_required) == 0);
    assert(start_required);
    assert(status.state == NRF_CS_CONTROL_STATE_RUNNING);
    assert(status.command_result == NRF_CS_CONTROL_RESULT_PENDING);

    start_required = true;
    assert(nrf_cs_control_runtime_accept_execute(&runtime, &request, &status, &start_required) == 0);
    assert(!start_required);
    assert(status.state == NRF_CS_CONTROL_STATE_RUNNING);

    start_required = true;
    assert(nrf_cs_control_runtime_accept_execute(&runtime, &other, &status, &start_required) == 0);
    assert(!start_required);
    assert(status.command_result == NRF_CS_CONTROL_RESULT_BUSY);
    assert(nrf_cs_control_runtime_select_result(&runtime, &other) == 0);
    assert(nrf_cs_control_runtime_take_selected(&runtime, &selected));
    assert(selected.command_result == NRF_CS_CONTROL_RESULT_BUSY);
    assert(nrf_cs_control_runtime_select_result(&runtime, &other) != 0);
    assert(nrf_cs_control_runtime_take_selected(&runtime, &selected));
    assert(selected.command_result == NRF_CS_CONTROL_RESULT_STALE);

    memset(&measurement, 0, sizeof(measurement));
    measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    measurement.distance_mm = 2222;
    measurement.confidence = 1000U;
    assert(nrf_cs_control_runtime_complete(&runtime, &request.step, &measurement) == 0);
    assert(nrf_cs_control_runtime_select_result(&runtime, &request) == 0);
    assert(nrf_cs_control_runtime_take_selected(&runtime, &selected));
    assert(selected.state == NRF_CS_CONTROL_STATE_COMPLETE);
    assert(selected.command_result == NRF_CS_CONTROL_RESULT_OK);
    assert(selected.measurement.distance_mm == 2222);

    start_required = true;
    assert(nrf_cs_control_runtime_accept_execute(&runtime, &invalid, &status, &start_required) == 0);
    assert(!start_required);
    assert(status.command_result == NRF_CS_CONTROL_RESULT_INVALID);
    assert(nrf_cs_control_runtime_select_result(&runtime, &invalid) == 0);
    assert(nrf_cs_control_runtime_take_selected(&runtime, &selected));
    assert(selected.command_result == NRF_CS_CONTROL_RESULT_INVALID);

    assert(nrf_cs_control_runtime_select_result(&runtime, &stale) != 0);
    assert(nrf_cs_control_runtime_take_selected(&runtime, &selected));
    assert(selected.command_result == NRF_CS_CONTROL_RESULT_STALE);
    return 0;
}
