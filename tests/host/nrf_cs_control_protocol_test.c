#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_control_protocol.h"

static nrf_cs_control_request_t make_request(uint32_t request_id)
{
    nrf_cs_control_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = request_id;
    request.step.cycle_id = 11U;
    request.step.membership_generation = 7U;
    request.step.attempt_id = 19U;
    request.step.pair_index = 3U;
    request.step.attempt_number = 2U;
    request.step.initiator = 2U;
    request.step.reflector = 5U;
    request.step.timeout_ms = 750U;
    return request;
}

int main(void)
{
    nrf_cs_control_request_t request = make_request(0x11223344U);
    nrf_cs_control_request_t decoded;
    nrf_cs_control_result_t result;
    nrf_cs_control_result_t decoded_result;
    uint8_t execute[NRF_CS_CONTROL_EXECUTE_LENGTH];
    uint8_t status[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    uint8_t result_frame[NRF_CS_CONTROL_RESULT_LENGTH];

    assert(NRF_CS_CONTROL_EXECUTE_LENGTH == 32U);
    assert(NRF_CS_CONTROL_STATUS_REQUEST_LENGTH == 32U);
    assert(NRF_CS_CONTROL_RESULT_LENGTH == 44U);

    assert(nrf_cs_control_encode_execute(&request, execute) == 0);
    memset(&decoded, 0, sizeof(decoded));
    assert(nrf_cs_control_decode_execute(execute, sizeof(execute), &decoded) == 0);
    assert(decoded.request_id == request.request_id);
    assert(nrf_cs_control_step_equal(&decoded.step, &request.step));

    assert(nrf_cs_control_encode_status_request(&request, status) == 0);
    memset(&decoded, 0, sizeof(decoded));
    assert(nrf_cs_control_decode_status_request(status, sizeof(status), &decoded) == 0);
    assert(decoded.request_id == request.request_id);
    assert(nrf_cs_control_step_equal(&decoded.step, &request.step));

    memset(&result, 0, sizeof(result));
    result.request_id = request.request_id;
    result.step = request.step;
    result.state = NRF_CS_CONTROL_STATE_COMPLETE;
    result.command_result = NRF_CS_CONTROL_RESULT_OK;
    result.measurement.outcome = NRF_CS_OUTCOME_SUCCESS;
    result.measurement.distance_mm = 1234;
    result.measurement.confidence = 1000U;
    result.measurement.provider_quality_flags = 0x12U;
    assert(nrf_cs_control_encode_result(&result, result_frame) == 0);
    memset(&decoded_result, 0, sizeof(decoded_result));
    assert(nrf_cs_control_decode_result(result_frame, sizeof(result_frame), &decoded_result) == 0);
    assert(decoded_result.request_id == request.request_id);
    assert(nrf_cs_control_step_equal(&decoded_result.step, &request.step));
    assert(decoded_result.state == NRF_CS_CONTROL_STATE_COMPLETE);
    assert(decoded_result.command_result == NRF_CS_CONTROL_RESULT_OK);
    assert(decoded_result.measurement.distance_mm == 1234);
    assert(decoded_result.measurement.confidence == 1000U);
    assert(decoded_result.measurement.provider_quality_flags == 0x12U);

    execute[12] ^= 0x01U;
    assert(nrf_cs_control_decode_execute(execute, sizeof(execute), &decoded) != 0);

    decoded = request;
    decoded.step.timeout_ms++;
    assert(!nrf_cs_control_step_equal(&decoded.step, &request.step));
    return 0;
}
