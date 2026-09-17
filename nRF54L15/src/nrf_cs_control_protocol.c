#include "nrf_cs_control_protocol.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "nrf_upstream_protocol.h"

#define MAGIC0 0x47U
#define MAGIC1 0x53U
#define STEP_OFFSET 10U

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8U); }
static uint32_t read_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U); }
static int32_t read_i32(const uint8_t *p) { const uint32_t v = read_le32(p); return v <= (uint32_t)INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v); }
static void write_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); }
static void write_le32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8U); p[2]=(uint8_t)(v>>16U); p[3]=(uint8_t)(v>>24U); }
static void write_i32(uint8_t *p, int32_t v) { write_le32(p, v >= 0 ? (uint32_t)v : UINT32_MAX - (uint32_t)(-(int64_t)v - 1)); }

static void write_header(uint8_t *frame, uint8_t type, uint16_t length, uint32_t request_id)
{
    frame[0] = MAGIC0; frame[1] = MAGIC1; frame[2] = NRF_UPSTREAM_PROTOCOL_VERSION; frame[3] = type;
    write_le16(&frame[4], length); write_le32(&frame[6], request_id);
}

static int validate_header(const uint8_t *frame, size_t length, uint8_t type, uint16_t expected)
{
    if (frame == NULL || length != expected || frame[0] != MAGIC0 || frame[1] != MAGIC1 ||
        frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION || frame[3] != type || read_le16(&frame[4]) != expected || read_le32(&frame[6]) == 0U) return -1;
    return nrf_upstream_protocol_crc16_ccitt_false(frame, expected - 2U) == read_le16(&frame[expected - 2U]) ? 0 : -2;
}

static void encode_step(uint8_t *p, const nrf_cs_step_t *s)
{
    write_le32(&p[0], s->cycle_id); write_le32(&p[4], s->membership_generation); write_le32(&p[8], s->attempt_id);
    p[12] = s->pair_index; p[13] = s->attempt_number; p[14] = s->initiator; p[15] = s->reflector; write_le32(&p[16], s->timeout_ms);
}

static void decode_step(const uint8_t *p, nrf_cs_step_t *s)
{
    s->cycle_id = read_le32(&p[0]); s->membership_generation = read_le32(&p[4]); s->attempt_id = read_le32(&p[8]);
    s->pair_index = p[12]; s->attempt_number = p[13]; s->initiator = p[14]; s->reflector = p[15]; s->timeout_ms = read_le32(&p[16]);
}

bool nrf_cs_control_step_equal(const nrf_cs_step_t *a, const nrf_cs_step_t *b)
{
    return a != NULL && b != NULL && a->cycle_id == b->cycle_id && a->membership_generation == b->membership_generation &&
        a->attempt_id == b->attempt_id && a->pair_index == b->pair_index && a->attempt_number == b->attempt_number &&
        a->initiator == b->initiator && a->reflector == b->reflector && a->timeout_ms == b->timeout_ms;
}

int nrf_cs_control_validate_request(const nrf_cs_control_request_t *request)
{
    if (request == NULL || request->request_id == 0U || request->step.cycle_id == 0U || request->step.membership_generation == 0U ||
        request->step.attempt_id == 0U || request->step.pair_index >= NRF_CS_MAX_PAIRS || request->step.attempt_number == 0U ||
        request->step.initiator == 0xFFU || request->step.reflector == 0xFFU || request->step.initiator == request->step.reflector || request->step.timeout_ms == 0U) return -1;
    return 0;
}

static int encode_request(const nrf_cs_control_request_t *request, uint8_t type, uint8_t frame[32])
{
    if (nrf_cs_control_validate_request(request) != 0 || frame == NULL) return -1;
    memset(frame, 0, 32U); write_header(frame, type, 32U, request->request_id); encode_step(&frame[STEP_OFFSET], &request->step);
    write_le16(&frame[30], nrf_upstream_protocol_crc16_ccitt_false(frame, 30U)); return 0;
}

static int decode_request(const uint8_t *frame, size_t length, uint8_t type, nrf_cs_control_request_t *request)
{
    if (request == NULL || validate_header(frame, length, type, 32U) != 0) return -1;
    memset(request, 0, sizeof(*request)); request->request_id = read_le32(&frame[6]); decode_step(&frame[STEP_OFFSET], &request->step);
    return nrf_cs_control_validate_request(request);
}

int nrf_cs_control_encode_execute(const nrf_cs_control_request_t *request, uint8_t frame[32]) { return encode_request(request, NRF_CS_CONTROL_MESSAGE_EXECUTE, frame); }
int nrf_cs_control_decode_execute(const uint8_t *frame, size_t length, nrf_cs_control_request_t *request) { return decode_request(frame, length, NRF_CS_CONTROL_MESSAGE_EXECUTE, request); }
int nrf_cs_control_encode_status_request(const nrf_cs_control_request_t *request, uint8_t frame[32]) { return encode_request(request, NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST, frame); }
int nrf_cs_control_decode_status_request(const uint8_t *frame, size_t length, nrf_cs_control_request_t *request) { return decode_request(frame, length, NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST, request); }

int nrf_cs_control_encode_result(const nrf_cs_control_result_t *result, uint8_t frame[44])
{
    nrf_cs_control_request_t request;
    if (result == NULL || frame == NULL) return -1;
    request.request_id = result->request_id; request.step = result->step;
    if (nrf_cs_control_validate_request(&request) != 0 || result->state > NRF_CS_CONTROL_STATE_FAILED || result->command_result > NRF_CS_CONTROL_RESULT_INTERNAL_ERROR ||
        result->measurement.outcome > NRF_CS_OUTCOME_OUTLIER || result->measurement.confidence > NRF_CS_CONFIDENCE_MAX ||
        (result->measurement.outcome == NRF_CS_OUTCOME_SUCCESS && result->measurement.distance_mm < 0)) return -1;
    memset(frame, 0, 44U); write_header(frame, NRF_CS_CONTROL_MESSAGE_RESULT, 44U, result->request_id); encode_step(&frame[STEP_OFFSET], &result->step);
    frame[30] = result->state; frame[31] = result->command_result; frame[32] = (uint8_t)result->measurement.outcome;
    write_le16(&frame[34], result->measurement.confidence); write_le16(&frame[36], result->measurement.provider_quality_flags); write_i32(&frame[38], result->measurement.distance_mm);
    write_le16(&frame[42], nrf_upstream_protocol_crc16_ccitt_false(frame, 42U)); return 0;
}

int nrf_cs_control_decode_result(const uint8_t *frame, size_t length, nrf_cs_control_result_t *result)
{
    nrf_cs_control_request_t request;
    if (result == NULL || validate_header(frame, length, NRF_CS_CONTROL_MESSAGE_RESULT, 44U) != 0) return -1;
    memset(result, 0, sizeof(*result)); result->request_id = read_le32(&frame[6]); decode_step(&frame[STEP_OFFSET], &result->step);
    request.request_id = result->request_id; request.step = result->step;
    result->state = frame[30]; result->command_result = frame[31]; result->measurement.outcome = (nrf_cs_outcome_t)frame[32];
    result->measurement.confidence = read_le16(&frame[34]); result->measurement.provider_quality_flags = read_le16(&frame[36]); result->measurement.distance_mm = read_i32(&frame[38]);
    if (nrf_cs_control_validate_request(&request) != 0 || result->state > NRF_CS_CONTROL_STATE_FAILED || result->command_result > NRF_CS_CONTROL_RESULT_INTERNAL_ERROR ||
        result->measurement.outcome > NRF_CS_OUTCOME_OUTLIER || result->measurement.confidence > NRF_CS_CONFIDENCE_MAX ||
        (result->measurement.outcome == NRF_CS_OUTCOME_SUCCESS && result->measurement.distance_mm < 0)) return -1;
    return 0;
}
