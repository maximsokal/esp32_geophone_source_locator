#include "nrf_cs_cycle_protocol.h"

#include <string.h>

#include <stdio.h>

#include "nrf_cs_distance_method.h"
#include "nrf_upstream_protocol.h"

#define MAGIC0 0x47U
#define MAGIC1 0x53U
#define HEADER_LENGTH 10U
#define NODE_NONE 0xFFU
#define PLAN_FLAG_FULL_ROTATION 0x01U

_Static_assert(NRF_CS_CYCLE_PLAN_STEPS_LENGTH <= 30U, "writes stay in one ESP32 FIFO segment");
_Static_assert(NRF_CS_CYCLE_RESULT_LENGTH <= NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH,
    "a result page must fit the read-window buffer");

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8U)); }
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static int32_t read_i32(const uint8_t *p)
{
    const uint32_t v = read_le32(p);
    return v <= (uint32_t)INT32_MAX ? (int32_t)v : -1 - (int32_t)(UINT32_MAX - v);
}
static void write_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); }
static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); p[2] = (uint8_t)(v >> 16U); p[3] = (uint8_t)(v >> 24U);
}
static void write_i32(uint8_t *p, int32_t v)
{
    write_le32(p, v >= 0 ? (uint32_t)v : UINT32_MAX - (uint32_t)(-(int64_t)v - 1));
}

static void begin_frame(uint8_t *frame, uint8_t type, uint16_t length, uint32_t request_id)
{
    memset(frame, 0, length);
    frame[0] = MAGIC0; frame[1] = MAGIC1; frame[2] = NRF_UPSTREAM_PROTOCOL_VERSION; frame[3] = type;
    write_le16(&frame[4], length);
    write_le32(&frame[6], request_id);
}

static void finish_frame(uint8_t *frame, uint16_t length)
{
    write_le16(&frame[length - 2U], nrf_upstream_protocol_crc16_ccitt_false(frame, length - 2U));
}

/* 0 when the frame has the expected type/length/request id and a valid CRC. */
static int check_frame(const uint8_t *frame, size_t length, uint8_t type, uint16_t expected)
{
    if (frame == NULL || length != expected || frame[0] != MAGIC0 || frame[1] != MAGIC1 ||
        frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION || frame[3] != type ||
        read_le16(&frame[4]) != expected || read_le32(&frame[6]) == 0U) {
        return -1;
    }
    return nrf_upstream_protocol_crc16_ccitt_false(frame, expected - 2U) == read_le16(&frame[expected - 2U])
        ? 0 : -2;
}

size_t nrf_cs_cycle_plan_chunk_count(size_t step_count)
{
    return (step_count + NRF_CS_CYCLE_STEPS_PER_CHUNK - 1U) / NRF_CS_CYCLE_STEPS_PER_CHUNK;
}

size_t nrf_cs_cycle_result_page_count(size_t pair_count)
{
    return (pair_count + NRF_CS_CYCLE_RESULTS_PER_PAGE - 1U) / NRF_CS_CYCLE_RESULTS_PER_PAGE;
}

static int plan_valid(const nrf_cs_cycle_plan_t *p)
{
    if (p == NULL || p->request_id == 0U || p->cycle_id == 0U || p->membership_generation == 0U ||
        p->pair_timeout_ms == 0U || p->max_attempts == 0U || p->samples_per_pair == 0U ||
        p->samples_per_pair > NRF_CS_MAX_SAMPLES_PER_EXECUTION || p->min_confidence > NRF_CS_CONFIDENCE_MAX ||
        (unsigned)p->samples_per_pair + p->max_attempts - 1U > NRF_CS_CYCLE_MAX_ATTEMPTS ||
        p->step_count == 0U || p->step_count > NRF_CS_CYCLE_MAX_STEPS || p->node_count < 2U ||
        p->node_count > NRF_CS_MAX_NODES || p->self_node == NODE_NONE) {
        return -1;
    }
    return 0;
}

int nrf_cs_cycle_encode_plan(const nrf_cs_cycle_plan_t *plan, uint8_t frame[NRF_CS_CYCLE_PLAN_LENGTH])
{
    if (frame == NULL || plan_valid(plan) != 0) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_PLAN, NRF_CS_CYCLE_PLAN_LENGTH, plan->request_id);
    write_le32(&frame[10], plan->cycle_id);
    write_le16(&frame[14], plan->membership_generation);
    write_le16(&frame[16], plan->pair_timeout_ms);
    frame[18] = plan->max_attempts;
    frame[19] = plan->samples_per_pair;
    write_le16(&frame[20], plan->min_confidence);
    frame[22] = plan->full_rotation ? PLAN_FLAG_FULL_ROTATION : 0U;
    frame[23] = plan->step_count;
    frame[24] = plan->node_count;
    frame[25] = plan->self_node;
    finish_frame(frame, NRF_CS_CYCLE_PLAN_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_plan(const uint8_t *frame, size_t length, nrf_cs_cycle_plan_t *plan)
{
    if (plan == NULL || check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_PLAN, NRF_CS_CYCLE_PLAN_LENGTH) != 0) return -1;
    memset(plan, 0, sizeof(*plan));
    plan->request_id = read_le32(&frame[6]);
    plan->cycle_id = read_le32(&frame[10]);
    plan->membership_generation = read_le16(&frame[14]);
    plan->pair_timeout_ms = read_le16(&frame[16]);
    plan->max_attempts = frame[18];
    plan->samples_per_pair = frame[19];
    plan->min_confidence = read_le16(&frame[20]);
    if ((frame[22] & ~PLAN_FLAG_FULL_ROTATION) != 0U) return -1; /* unknown flag bits */
    plan->full_rotation = (frame[22] & PLAN_FLAG_FULL_ROTATION) != 0U;
    plan->step_count = frame[23];
    plan->node_count = frame[24];
    plan->self_node = frame[25];
    return plan_valid(plan);
}

static int step_valid(const nrf_cs_cycle_step_t *s)
{
    return s->initiator == NODE_NONE || s->reflector == NODE_NONE || s->initiator == s->reflector ? -1 : 0;
}

static int steps_valid(const nrf_cs_cycle_plan_steps_t *c)
{
    if (c == NULL || c->request_id == 0U || c->cycle_id == 0U || c->step_count == 0U ||
        c->step_count > NRF_CS_CYCLE_STEPS_PER_CHUNK ||
        (size_t)c->first_step + c->step_count > NRF_CS_CYCLE_MAX_STEPS) {
        return -1;
    }
    for (uint8_t i = 0U; i < c->step_count; ++i) {
        if (step_valid(&c->steps[i]) != 0) return -1;
    }
    return 0;
}

int nrf_cs_cycle_encode_plan_steps(const nrf_cs_cycle_plan_steps_t *chunk, uint8_t frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH])
{
    if (frame == NULL || steps_valid(chunk) != 0) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_PLAN_STEPS, NRF_CS_CYCLE_PLAN_STEPS_LENGTH, chunk->request_id);
    write_le32(&frame[10], chunk->cycle_id);
    frame[14] = chunk->first_step;
    frame[15] = chunk->step_count;
    for (uint8_t i = 0U; i < chunk->step_count; ++i) {
        frame[16U + 2U * i] = chunk->steps[i].initiator;
        frame[17U + 2U * i] = chunk->steps[i].reflector;
    }
    finish_frame(frame, NRF_CS_CYCLE_PLAN_STEPS_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_plan_steps(const uint8_t *frame, size_t length, nrf_cs_cycle_plan_steps_t *chunk)
{
    if (chunk == NULL ||
        check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_PLAN_STEPS, NRF_CS_CYCLE_PLAN_STEPS_LENGTH) != 0) {
        return -1;
    }
    memset(chunk, 0, sizeof(*chunk));
    chunk->request_id = read_le32(&frame[6]);
    chunk->cycle_id = read_le32(&frame[10]);
    chunk->first_step = frame[14];
    chunk->step_count = frame[15];
    if (chunk->step_count > NRF_CS_CYCLE_STEPS_PER_CHUNK) return -1;
    for (uint8_t i = 0U; i < chunk->step_count; ++i) {
        chunk->steps[i].initiator = frame[16U + 2U * i];
        chunk->steps[i].reflector = frame[17U + 2U * i];
    }
    return steps_valid(chunk);
}

int nrf_cs_cycle_encode_go(const nrf_cs_cycle_go_t *go, uint8_t frame[NRF_CS_CYCLE_GO_LENGTH])
{
    if (frame == NULL || go == NULL || go->request_id == 0U || go->cycle_id == 0U || go->total_timeout_ms == 0U) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_GO, NRF_CS_CYCLE_GO_LENGTH, go->request_id);
    write_le32(&frame[10], go->cycle_id);
    write_le32(&frame[14], go->total_timeout_ms);
    finish_frame(frame, NRF_CS_CYCLE_GO_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_go(const uint8_t *frame, size_t length, nrf_cs_cycle_go_t *go)
{
    if (go == NULL || check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_GO, NRF_CS_CYCLE_GO_LENGTH) != 0) return -1;
    go->request_id = read_le32(&frame[6]);
    go->cycle_id = read_le32(&frame[10]);
    go->total_timeout_ms = read_le32(&frame[14]);
    return go->cycle_id == 0U || go->total_timeout_ms == 0U ? -1 : 0;
}

static int result_request_valid(const nrf_cs_cycle_result_request_t *r)
{
    return r == NULL || r->request_id == 0U || r->cycle_id == 0U ||
        (r->page != NRF_CS_CYCLE_RESULT_RELEASE_PAGE &&
         r->page >= nrf_cs_cycle_result_page_count(NRF_CS_CYCLE_MAX_RECORDS)) ? -1 : 0;
}

int nrf_cs_cycle_encode_result_request(const nrf_cs_cycle_result_request_t *request, uint8_t frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH])
{
    if (frame == NULL || result_request_valid(request) != 0) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST, NRF_CS_CYCLE_RESULT_REQUEST_LENGTH, request->request_id);
    write_le32(&frame[10], request->cycle_id);
    frame[14] = request->page;
    finish_frame(frame, NRF_CS_CYCLE_RESULT_REQUEST_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_result_request(const uint8_t *frame, size_t length, nrf_cs_cycle_result_request_t *request)
{
    if (request == NULL ||
        check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_RESULT_REQUEST, NRF_CS_CYCLE_RESULT_REQUEST_LENGTH) != 0 ||
        frame[15] != 0U) {
        return -1;
    }
    request->request_id = read_le32(&frame[6]);
    request->cycle_id = read_le32(&frame[10]);
    request->page = frame[14];
    return result_request_valid(request);
}

static int status_valid(const nrf_cs_cycle_status_t *s)
{
    return s == NULL || s->request_id == 0U || s->state > NRF_CS_CYCLE_STATE_FAILED ||
        s->failure > NRF_CS_CYCLE_FAILURE_ABORTED || s->pairs_total > NRF_CS_CYCLE_MAX_STEPS ||
        s->pairs_done > s->pairs_total ||
        (s->state != NRF_CS_CYCLE_STATE_IDLE && s->cycle_id == 0U) ? -1 : 0;
}

int nrf_cs_cycle_encode_status(const nrf_cs_cycle_status_t *status, uint8_t frame[NRF_CS_CYCLE_STATUS_LENGTH])
{
    if (frame == NULL || status_valid(status) != 0) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_STATUS, NRF_CS_CYCLE_STATUS_LENGTH, status->request_id);
    write_le32(&frame[10], status->cycle_id);
    frame[14] = status->state;
    frame[15] = status->failure;
    frame[16] = status->pairs_done;
    frame[17] = status->pairs_total;
    write_le32(&frame[18], status->elapsed_ms);
    finish_frame(frame, NRF_CS_CYCLE_STATUS_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_status(const uint8_t *frame, size_t length, nrf_cs_cycle_status_t *status)
{
    if (status == NULL ||
        check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_STATUS, NRF_CS_CYCLE_STATUS_LENGTH) != 0) {
        return -1;
    }
    memset(status, 0, sizeof(*status));
    status->request_id = read_le32(&frame[6]);
    status->cycle_id = read_le32(&frame[10]);
    status->state = frame[14];
    status->failure = frame[15];
    status->pairs_done = frame[16];
    status->pairs_total = frame[17];
    status->elapsed_ms = read_le32(&frame[18]);
    return status_valid(status);
}

static int record_valid(const nrf_cs_cycle_record_t *r)
{
    return r->execution >= NRF_CS_CYCLE_MAX_STEPS || r->attempt == 0U || r->attempt > NRF_CS_CYCLE_MAX_ATTEMPTS ||
        r->outcome > NRF_CS_OUTCOME_OUTLIER || r->confidence > NRF_CS_CONFIDENCE_MAX ||
        (r->outcome == NRF_CS_OUTCOME_SUCCESS && r->distance_mm < 0) ? -1 : 0;
}

static int result_valid(const nrf_cs_cycle_result_t *r)
{
    if (r == NULL || r->request_id == 0U || r->cycle_id == 0U ||
        r->record_total > NRF_CS_CYCLE_MAX_RECORDS ||
        r->page_count != nrf_cs_cycle_result_page_count(r->record_total) || r->page_count == 0U ||
        r->page >= r->page_count) {
        return -1;
    }
    const size_t before = (size_t)r->page * NRF_CS_CYCLE_RESULTS_PER_PAGE;
    const size_t left = r->record_total - before;
    const size_t expected = left < NRF_CS_CYCLE_RESULTS_PER_PAGE ? left : NRF_CS_CYCLE_RESULTS_PER_PAGE;
    if (r->record_count != expected) return -1;
    for (uint8_t i = 0U; i < r->record_count; ++i) {
        if (record_valid(&r->records[i]) != 0) return -1;
    }
    return 0;
}

int nrf_cs_cycle_encode_result(const nrf_cs_cycle_result_t *result, uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH])
{
    if (frame == NULL || result_valid(result) != 0) return -1;
    begin_frame(frame, NRF_CS_CYCLE_MESSAGE_RESULT, NRF_CS_CYCLE_RESULT_LENGTH, result->request_id);
    write_le32(&frame[10], result->cycle_id);
    frame[14] = result->page;
    frame[15] = result->page_count;
    frame[16] = result->record_total;
    frame[17] = result->record_count;
    for (uint8_t i = 0U; i < result->record_count; ++i) {
        const nrf_cs_cycle_record_t *r = &result->records[i];
        uint8_t *p = &frame[18U + (size_t)i * NRF_CS_CYCLE_RESULT_RECORD_LENGTH];
        p[0] = r->execution; p[1] = r->attempt; p[2] = r->outcome;
        write_i32(&p[4], r->distance_mm);
        write_le16(&p[8], r->confidence);
        write_le16(&p[10], r->provider_quality_flags);
    }
    finish_frame(frame, NRF_CS_CYCLE_RESULT_LENGTH);
    return 0;
}

int nrf_cs_cycle_decode_result(const uint8_t *frame, size_t length, nrf_cs_cycle_result_t *result)
{
    if (result == NULL ||
        check_frame(frame, length, NRF_CS_CYCLE_MESSAGE_RESULT, NRF_CS_CYCLE_RESULT_LENGTH) != 0) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    result->request_id = read_le32(&frame[6]);
    result->cycle_id = read_le32(&frame[10]);
    result->page = frame[14];
    result->page_count = frame[15];
    result->record_total = frame[16];
    result->record_count = frame[17];
    if (result->record_count > NRF_CS_CYCLE_RESULTS_PER_PAGE) return -1;
    for (uint8_t i = 0U; i < result->record_count; ++i) {
        nrf_cs_cycle_record_t *r = &result->records[i];
        const uint8_t *p = &frame[18U + (size_t)i * NRF_CS_CYCLE_RESULT_RECORD_LENGTH];
        if (p[3] != 0U) return -1;
        r->execution = p[0]; r->attempt = p[1]; r->outcome = p[2];
        r->distance_mm = read_i32(&p[4]);
        r->confidence = read_le16(&p[8]);
        r->provider_quality_flags = read_le16(&p[10]);
    }
    return result_valid(result);
}

void nrf_cs_cycle_record_from_measurement(
    uint8_t execution, uint8_t attempt, const nrf_cs_measurement_t *measurement, nrf_cs_cycle_record_t *record)
{
    if (measurement == NULL || record == NULL) return;
    record->execution = execution;
    record->attempt = attempt;
    record->outcome = (uint8_t)measurement->outcome;
    record->distance_mm = measurement->distance_mm;
    record->confidence = measurement->confidence;
    record->provider_quality_flags = measurement->provider_quality_flags;
}

void nrf_cs_cycle_record_to_measurement(const nrf_cs_cycle_record_t *record, nrf_cs_measurement_t *measurement)
{
    if (measurement == NULL || record == NULL) return;
    measurement->outcome = (nrf_cs_outcome_t)record->outcome;
    measurement->distance_mm = record->distance_mm;
    measurement->confidence = record->confidence;
    measurement->provider_quality_flags = record->provider_quality_flags;
}

static const char *outcome_text(uint8_t outcome)
{
    switch (outcome) {
        case NRF_CS_OUTCOME_SUCCESS: return "SUCCESS";
        case NRF_CS_OUTCOME_TIMEOUT: return "TIMEOUT";
        case NRF_CS_OUTCOME_TRANSPORT_ERROR: return "TRANSPORT_ERROR";
        case NRF_CS_OUTCOME_OUTLIER: return "OUTLIER";
        default: return "?";
    }
}

int nrf_cs_cycle_record_describe(const nrf_cs_cycle_record_t *record, char *line, size_t capacity)
{
    if (record == NULL || line == NULL || capacity == 0U) return -1;
    /* Bits 1..3 are NRF_CS_PROVIDER_QUALITY_TONE_OK / PBR_VALID / RTT_VALID (nrf_cs_pair_runtime_zephyr.h). */
    return snprintf(line, capacity, "exec=%u attempt=%u outcome=%s dist_mm=%d conf=%u method=%s tone_ok=%u pbr=%u rtt=%u",
        (unsigned)record->execution, (unsigned)record->attempt, outcome_text(record->outcome), (int)record->distance_mm,
        (unsigned)record->confidence, nrf_cs_distance_method_name(nrf_cs_distance_method_from_flags(record->provider_quality_flags)),
        (unsigned)((record->provider_quality_flags >> 1U) & 1U), (unsigned)((record->provider_quality_flags >> 2U) & 1U),
        (unsigned)((record->provider_quality_flags >> 3U) & 1U));
}
