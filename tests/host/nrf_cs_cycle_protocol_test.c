#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nrf_cs_cycle_protocol.h"
#include "nrf_upstream_protocol.h"

static nrf_cs_cycle_plan_t make_plan(void)
{
    return (nrf_cs_cycle_plan_t){
        .request_id = 7U, .cycle_id = 0x01020304U, .membership_generation = 3U,
        .pair_timeout_ms = 500U, .max_attempts = 3U, .samples_per_pair = 3U,
        .min_confidence = 500U, .full_rotation = true, .step_count = 20U,
        .node_count = 5U, .self_node = 2U,
    };
}

static nrf_cs_cycle_record_t make_record(uint8_t execution, uint8_t attempt)
{
    return (nrf_cs_cycle_record_t){
        .execution = execution, .attempt = attempt, .outcome = NRF_CS_OUTCOME_SUCCESS,
        .distance_mm = 1234 + execution, .confidence = 900U, .provider_quality_flags = 0x0102U,
    };
}

/* A corrupted byte anywhere must be rejected, never decoded into different values. */
static void assert_every_byte_is_protected(const uint8_t *frame, size_t length,
    int (*decode)(const uint8_t *, size_t, void *), void *scratch)
{
    uint8_t copy[NRF_CS_CYCLE_RESULT_LENGTH];
    assert(length <= sizeof(copy));
    assert(decode(frame, length, scratch) == 0);
    for (size_t i = 0U; i < length; ++i) {
        memcpy(copy, frame, length);
        copy[i] ^= 0x10U;
        assert(decode(copy, length, scratch) != 0);
    }
    assert(decode(frame, length - 1U, scratch) != 0);
    assert(decode(NULL, length, scratch) != 0);
}

static int decode_plan_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_plan(f, n, o); }
static int decode_steps_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_plan_steps(f, n, o); }
static int decode_go_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_go(f, n, o); }
static int decode_req_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_result_request(f, n, o); }
static int decode_status_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_status(f, n, o); }
static int decode_result_w(const uint8_t *f, size_t n, void *o) { return nrf_cs_cycle_decode_result(f, n, o); }

static void test_frame_sizes_keep_writes_in_one_fifo_segment(void)
{
    /* Frames the ESP32 writes: 30 bytes or less, because 32 byte writes were truncated on the bench. */
    assert(NRF_CS_CYCLE_PLAN_LENGTH <= 30U && NRF_CS_CYCLE_PLAN_STEPS_LENGTH <= 30U);
    assert(NRF_CS_CYCLE_GO_LENGTH <= 30U && NRF_CS_CYCLE_RESULT_REQUEST_LENGTH <= 30U);
    /* Replies are read as 28 byte windows; the status is exactly one window. */
    assert(NRF_CS_CYCLE_STATUS_LENGTH == 28U);
    assert(NRF_CS_CYCLE_RESULT_LENGTH == 116U);
    assert(NRF_CS_CYCLE_RESULT_LENGTH <= NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    assert(nrf_cs_cycle_plan_chunk_count(20U) == 4U && nrf_cs_cycle_plan_chunk_count(6U) == 1U);
    assert(nrf_cs_cycle_plan_chunk_count(7U) == 2U && nrf_cs_cycle_plan_chunk_count(0U) == 0U);
    assert(nrf_cs_cycle_result_page_count(10U) == 2U && nrf_cs_cycle_result_page_count(8U) == 1U);
    assert(nrf_cs_cycle_result_page_count(9U) == 2U && nrf_cs_cycle_result_page_count(0U) == 0U);
    assert(NRF_CS_CYCLE_MAX_RECORDS == 240U && nrf_cs_cycle_result_page_count(NRF_CS_CYCLE_MAX_RECORDS) == 30U);
}

static void test_plan_round_trip_wire_bytes_and_validation(void)
{
    uint8_t frame[NRF_CS_CYCLE_PLAN_LENGTH];
    nrf_cs_cycle_plan_t plan = make_plan();
    nrf_cs_cycle_plan_t decoded;

    assert(nrf_cs_cycle_encode_plan(&plan, frame) == 0);
    /* Pin the wire format: magic 'G' 'S', version, type 0x0B, length 28, request id, cycle id. */
    assert(frame[0] == 0x47U && frame[1] == 0x53U && frame[2] == NRF_UPSTREAM_PROTOCOL_VERSION);
    assert(frame[3] == 0x0BU && frame[4] == 28U && frame[5] == 0U);
    assert(frame[6] == 7U && frame[10] == 0x04U && frame[13] == 0x01U);
    assert(frame[14] == 3U && frame[16] == 0xF4U && frame[17] == 0x01U);
    assert(frame[18] == 3U && frame[19] == 3U && frame[22] == 1U);
    assert(frame[23] == 20U && frame[24] == 5U && frame[25] == 2U);
    assert(nrf_cs_cycle_decode_plan(frame, sizeof(frame), &decoded) == 0);
    assert(memcmp(&decoded, &plan, sizeof(plan)) == 0);
    assert_every_byte_is_protected(frame, sizeof(frame), decode_plan_w, &decoded);

    plan.full_rotation = false;
    assert(nrf_cs_cycle_encode_plan(&plan, frame) == 0);
    assert(nrf_cs_cycle_decode_plan(frame, sizeof(frame), &decoded) == 0 && !decoded.full_rotation);

    nrf_cs_cycle_plan_t bad;
    bad = make_plan(); bad.request_id = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.cycle_id = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.membership_generation = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.pair_timeout_ms = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.max_attempts = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.samples_per_pair = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.samples_per_pair = NRF_CS_MAX_SAMPLES_PER_EXECUTION + 1U;
    assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.min_confidence = NRF_CS_CONFIDENCE_MAX + 1U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    /* samples + max_attempts - 1 attempts must fit the node's per-execution record store. */
    bad = make_plan(); bad.samples_per_pair = 8U; bad.max_attempts = 5U; assert(nrf_cs_cycle_encode_plan(&bad, frame) == 0);
    bad = make_plan(); bad.samples_per_pair = 8U; bad.max_attempts = 6U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.step_count = 0U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.step_count = NRF_CS_CYCLE_MAX_STEPS + 1U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.node_count = 1U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.node_count = NRF_CS_MAX_NODES + 1U; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    bad = make_plan(); bad.self_node = 0xFFU; assert(nrf_cs_cycle_encode_plan(&bad, frame) != 0);
    assert(nrf_cs_cycle_encode_plan(NULL, frame) != 0 && nrf_cs_cycle_encode_plan(&plan, NULL) != 0);

    /* A frame with an unknown flag bit is refused even with a valid CRC. */
    assert(nrf_cs_cycle_encode_plan(&plan, frame) == 0);
    frame[22] = 0x02U;
    {
        const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, NRF_CS_CYCLE_PLAN_LENGTH - 2U);
        frame[26] = (uint8_t)crc; frame[27] = (uint8_t)(crc >> 8U);
    }
    assert(nrf_cs_cycle_decode_plan(frame, sizeof(frame), &decoded) != 0);
}

static void test_plan_steps_chunks(void)
{
    uint8_t frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH];
    nrf_cs_cycle_plan_steps_t chunk = {.request_id = 9U, .cycle_id = 5U, .first_step = 18U, .step_count = 2U};
    nrf_cs_cycle_plan_steps_t decoded;

    chunk.steps[0] = (nrf_cs_cycle_step_t){.initiator = 4U, .reflector = 0U};
    chunk.steps[1] = (nrf_cs_cycle_step_t){.initiator = 3U, .reflector = 1U};
    assert(nrf_cs_cycle_encode_plan_steps(&chunk, frame) == 0);
    assert(frame[3] == 0x0CU && frame[4] == 30U && frame[14] == 18U && frame[15] == 2U);
    assert(frame[16] == 4U && frame[17] == 0U && frame[18] == 3U && frame[19] == 1U);
    assert(frame[20] == 0U && frame[27] == 0U); /* unused slots stay zero */
    assert(nrf_cs_cycle_decode_plan_steps(frame, sizeof(frame), &decoded) == 0);
    assert(decoded.first_step == 18U && decoded.step_count == 2U);
    assert(decoded.steps[1].initiator == 3U && decoded.steps[1].reflector == 1U);
    assert_every_byte_is_protected(frame, sizeof(frame), decode_steps_w, &decoded);

    nrf_cs_cycle_plan_steps_t bad = chunk;
    bad.first_step = 19U; /* 19 + 2 > 20 */
    assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);
    bad = chunk; bad.step_count = 0U; assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);
    bad = chunk; bad.step_count = NRF_CS_CYCLE_STEPS_PER_CHUNK + 1U;
    assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);
    bad = chunk; bad.steps[1].reflector = bad.steps[1].initiator; assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);
    bad = chunk; bad.steps[0].initiator = 0xFFU; assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);
    bad = chunk; bad.cycle_id = 0U; assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) != 0);

    /* The last chunk of a full 20 step plan. */
    bad = chunk; bad.first_step = 18U; bad.step_count = 2U;
    assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) == 0);
    bad.first_step = 14U; bad.step_count = 6U;
    for (uint8_t i = 0U; i < 6U; ++i) bad.steps[i] = (nrf_cs_cycle_step_t){.initiator = (uint8_t)i, .reflector = (uint8_t)(i + 1U)};
    assert(nrf_cs_cycle_encode_plan_steps(&bad, frame) == 0);
    assert(nrf_cs_cycle_decode_plan_steps(frame, sizeof(frame), &decoded) == 0 && decoded.step_count == 6U);
}

static void test_go_and_result_request(void)
{
    uint8_t go_frame[NRF_CS_CYCLE_GO_LENGTH];
    uint8_t req_frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];
    nrf_cs_cycle_go_t go = {.request_id = 11U, .cycle_id = 77U, .total_timeout_ms = 90000U};
    nrf_cs_cycle_go_t go_out;
    nrf_cs_cycle_result_request_t req = {.request_id = 12U, .cycle_id = 77U, .page = 2U};
    nrf_cs_cycle_result_request_t req_out;

    assert(nrf_cs_cycle_encode_go(&go, go_frame) == 0);
    assert(go_frame[3] == 0x0DU && go_frame[4] == 20U);
    assert(nrf_cs_cycle_decode_go(go_frame, sizeof(go_frame), &go_out) == 0);
    assert(go_out.request_id == 11U && go_out.cycle_id == 77U && go_out.total_timeout_ms == 90000U);
    assert_every_byte_is_protected(go_frame, sizeof(go_frame), decode_go_w, &go_out);
    go.total_timeout_ms = 0U; assert(nrf_cs_cycle_encode_go(&go, go_frame) != 0);
    go.total_timeout_ms = 1U; go.cycle_id = 0U; assert(nrf_cs_cycle_encode_go(&go, go_frame) != 0);
    go.cycle_id = 1U; go.request_id = 0U; assert(nrf_cs_cycle_encode_go(&go, go_frame) != 0);

    assert(nrf_cs_cycle_encode_result_request(&req, req_frame) == 0);
    assert(req_frame[3] == 0x0EU && req_frame[4] == 18U && req_frame[14] == 2U);
    assert(nrf_cs_cycle_decode_result_request(req_frame, sizeof(req_frame), &req_out) == 0);
    assert(req_out.page == 2U && req_out.cycle_id == 77U);
    assert_every_byte_is_protected(req_frame, sizeof(req_frame), decode_req_w, &req_out);
    req.page = 29U; /* the last page of a worst-case 240 record store */
    assert(nrf_cs_cycle_encode_result_request(&req, req_frame) == 0);
    req.page = 30U;
    assert(nrf_cs_cycle_encode_result_request(&req, req_frame) != 0);
    /* 0xFF releases the node's reply so normal STATUS reads work again. */
    req.page = NRF_CS_CYCLE_RESULT_RELEASE_PAGE;
    assert(nrf_cs_cycle_encode_result_request(&req, req_frame) == 0);
    assert(nrf_cs_cycle_decode_result_request(req_frame, sizeof(req_frame), &req_out) == 0);
    assert(req_out.page == NRF_CS_CYCLE_RESULT_RELEASE_PAGE);
}

static void test_status(void)
{
    uint8_t frame[NRF_CS_CYCLE_STATUS_LENGTH];
    nrf_cs_cycle_status_t status = {
        .request_id = 11U, .cycle_id = 77U, .state = NRF_CS_CYCLE_STATE_RUNNING,
        .failure = NRF_CS_CYCLE_FAILURE_NONE, .pairs_done = 6U, .pairs_total = 20U, .elapsed_ms = 41500U,
    };
    nrf_cs_cycle_status_t out;

    assert(nrf_cs_cycle_encode_status(&status, frame) == 0);
    assert(frame[3] == 0x85U && frame[4] == 28U && frame[14] == 1U && frame[16] == 6U && frame[17] == 20U);
    assert(nrf_cs_cycle_decode_status(frame, sizeof(frame), &out) == 0);
    assert(memcmp(&out, &status, sizeof(out)) == 0);
    assert_every_byte_is_protected(frame, sizeof(frame), decode_status_w, &out);

    status.state = NRF_CS_CYCLE_STATE_COMPLETE; status.pairs_done = 20U;
    assert(nrf_cs_cycle_encode_status(&status, frame) == 0);
    status.state = NRF_CS_CYCLE_STATE_FAILED; status.failure = NRF_CS_CYCLE_FAILURE_TIMEOUT;
    assert(nrf_cs_cycle_encode_status(&status, frame) == 0);

    nrf_cs_cycle_status_t bad = status;
    bad.state = NRF_CS_CYCLE_STATE_FAILED + 1U; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
    bad = status; bad.failure = NRF_CS_CYCLE_FAILURE_ABORTED + 1U; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
    bad = status; bad.pairs_done = 21U; bad.pairs_total = 20U; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
    bad = status; bad.pairs_total = NRF_CS_CYCLE_MAX_STEPS + 1U; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
    bad = status; bad.request_id = 0U; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
    /* An idle node has no cycle id yet. */
    bad = (nrf_cs_cycle_status_t){.request_id = 1U, .state = NRF_CS_CYCLE_STATE_IDLE};
    assert(nrf_cs_cycle_encode_status(&bad, frame) == 0);
    bad.state = NRF_CS_CYCLE_STATE_RUNNING; assert(nrf_cs_cycle_encode_status(&bad, frame) != 0);
}

static nrf_cs_cycle_result_t make_result(uint8_t record_total, uint8_t page)
{
    nrf_cs_cycle_result_t result = {
        .request_id = 13U, .cycle_id = 77U, .page = page,
        .page_count = (uint8_t)nrf_cs_cycle_result_page_count(record_total), .record_total = record_total,
    };
    const size_t before = (size_t)page * NRF_CS_CYCLE_RESULTS_PER_PAGE;
    const size_t left = record_total - before;
    result.record_count = (uint8_t)(left < NRF_CS_CYCLE_RESULTS_PER_PAGE ? left : NRF_CS_CYCLE_RESULTS_PER_PAGE);
    for (uint8_t i = 0U; i < result.record_count; ++i) {
        result.records[i] = make_record((uint8_t)((before + i) / 6U), (uint8_t)(1U + (before + i) % 6U));
    }
    return result;
}

static void assert_records_equal(const nrf_cs_cycle_record_t *a, const nrf_cs_cycle_record_t *b)
{
    assert(a->execution == b->execution && a->attempt == b->attempt && a->outcome == b->outcome);
    assert(a->distance_mm == b->distance_mm && a->confidence == b->confidence);
    assert(a->provider_quality_flags == b->provider_quality_flags);
}

static void test_result_pages_round_trip(void)
{
    uint8_t frame[NRF_CS_CYCLE_RESULT_LENGTH];
    nrf_cs_cycle_result_t out;

    for (uint8_t page = 0U; page < 3U; ++page) {
        nrf_cs_cycle_result_t result = make_result(20U, page);
        assert(nrf_cs_cycle_encode_result(&result, frame) == 0);
        assert(frame[3] == 0x86U && frame[4] == 116U && frame[5] == 0U);
        assert(nrf_cs_cycle_decode_result(frame, sizeof(frame), &out) == 0);
        assert(out.request_id == result.request_id && out.cycle_id == result.cycle_id);
        assert(out.page == page && out.page_count == 3U && out.record_total == 20U);
        assert(out.record_count == (page == 2U ? 4U : 8U));
        for (uint8_t i = 0U; i < out.record_count; ++i) assert_records_equal(&out.records[i], &result.records[i]);
    }
    nrf_cs_cycle_result_t full = make_result(20U, 0U);
    assert(nrf_cs_cycle_encode_result(&full, frame) == 0);
    assert_every_byte_is_protected(frame, sizeof(frame), decode_result_w, &out);

    /* An empty node still answers with a valid page: no records, one page. */
    nrf_cs_cycle_result_t empty = {.request_id = 14U, .cycle_id = 77U, .page = 0U, .page_count = 0U, .record_total = 0U};
    assert(nrf_cs_cycle_encode_result(&empty, frame) != 0); /* zero pages are not representable; callers send none */

    /* Failure outcomes and large distances survive; the reserved byte stays zero. */
    full.records[0].outcome = NRF_CS_OUTCOME_TIMEOUT; full.records[0].distance_mm = -1;
    full.records[1].distance_mm = INT32_MAX; full.records[1].confidence = NRF_CS_CONFIDENCE_MAX;
    assert(nrf_cs_cycle_encode_result(&full, frame) == 0);
    assert(frame[18U + 3U] == 0U);
    assert(nrf_cs_cycle_decode_result(frame, sizeof(frame), &out) == 0);
    assert(out.records[0].outcome == NRF_CS_OUTCOME_TIMEOUT && out.records[0].distance_mm == -1);
    assert(out.records[1].distance_mm == INT32_MAX && out.records[1].confidence == NRF_CS_CONFIDENCE_MAX);

    nrf_cs_cycle_result_t bad = make_result(20U, 1U);
    bad.record_count = 7U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);   /* page 1 holds 8 */
    bad = make_result(20U, 0U); bad.page_count = 2U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.page = 3U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.record_total = 241U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[2].attempt = 0U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[2].attempt = NRF_CS_CYCLE_MAX_ATTEMPTS + 1U;
    assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[1].execution = NRF_CS_CYCLE_MAX_STEPS; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[1].outcome = NRF_CS_OUTCOME_OUTLIER + 1U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[1].confidence = NRF_CS_CONFIDENCE_MAX + 1U; assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);
    bad = make_result(20U, 0U); bad.records[1].distance_mm = -5; /* a success cannot be negative */
    assert(nrf_cs_cycle_encode_result(&bad, frame) != 0);

    /* A nonzero reserved byte is refused even with a valid CRC. */
    assert(nrf_cs_cycle_encode_result(&full, frame) == 0);
    frame[18U + 3U] = 1U;
    {
        const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, NRF_CS_CYCLE_RESULT_LENGTH - 2U);
        frame[NRF_CS_CYCLE_RESULT_LENGTH - 2U] = (uint8_t)crc;
        frame[NRF_CS_CYCLE_RESULT_LENGTH - 1U] = (uint8_t)(crc >> 8U);
    }
    assert(nrf_cs_cycle_decode_result(frame, sizeof(frame), &out) != 0);
}

static void test_measurement_record_conversion(void)
{
    const nrf_cs_measurement_t measurement = {
        .outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 2500, .confidence = 800U, .provider_quality_flags = 3U,
    };
    nrf_cs_cycle_record_t record;
    nrf_cs_measurement_t back;

    nrf_cs_cycle_record_from_measurement(7U, 2U, &measurement, &record);
    assert(record.execution == 7U && record.attempt == 2U && record.outcome == NRF_CS_OUTCOME_SUCCESS);
    nrf_cs_cycle_record_to_measurement(&record, &back);
    assert(back.outcome == NRF_CS_OUTCOME_SUCCESS && back.distance_mm == 2500);
    assert(back.confidence == 800U && back.provider_quality_flags == 3U);
    nrf_cs_cycle_record_from_measurement(0U, 1U, NULL, &record);
    nrf_cs_cycle_record_to_measurement(NULL, &back);
}

int main(void)
{
    test_frame_sizes_keep_writes_in_one_fifo_segment();
    test_plan_round_trip_wire_bytes_and_validation();
    test_plan_steps_chunks();
    test_go_and_result_request();
    test_status();
    test_result_pages_round_trip();
    test_measurement_record_conversion();
    puts("nRF CS cycle protocol: PASS");
    return 0;
}
