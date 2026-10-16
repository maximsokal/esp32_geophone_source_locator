#define main max494_existing_runtime_test_main
#include "nrf_bootstrap_runtime_zephyr_test.c"
#undef main

static void refresh_assign_crc(uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH])
{
    const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 22U);
    frame[22] = (uint8_t)crc;
    frame[23] = (uint8_t)(crc >> 8U);
}

static void send_cs_execute_request(const nrf_cs_control_request_t *request)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    assert(nrf_cs_control_encode_execute(request, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static nrf_cs_control_result_t read_cs_result(void)
{
    uint32_t length = 0U;
    uint8_t *frame = read_frame(&length);
    nrf_cs_control_result_t result;
    assert(length == NRF_CS_CONTROL_RESULT_LENGTH);
    assert(nrf_cs_control_decode_result(frame, length, &result) == 0);
    return result;
}

static nrf_cs_control_request_t model_cs_request(uint32_t request_id, uint32_t attempt_id)
{
    nrf_cs_control_request_t request = {0};
    request.request_id = request_id;
    request.step.cycle_id = 1U;
    request.step.membership_generation = 1U;
    request.step.attempt_id = attempt_id;
    request.step.pair_index = 0U;
    request.step.attempt_number = 1U;
    request.step.initiator = 2U;
    request.step.reflector = 3U;
    request.step.timeout_ms = 500U;
    return request;
}

/* characterization: bounded ASSIGN queue returns BUSY for the rejected id, then publishes the last queued id. */
static void test_assign_queue_model_and_request_correlation(void)
{
    nrf_upstream_assign_t assign = make_assign(0x900U);
    uint32_t queued_ids[4];
    size_t model_count = 0U;
    for (size_t index = 0U; index < 4U; ++index) {
        assign.request_id = 0x900U + (uint32_t)index;
        queued_ids[model_count++] = assign.request_id;
        send_assign(&assign);
        assert(model_count <= 4U);
    }
    assign.request_id = 0x904U;
    send_assign(&assign);
    nrf_upstream_status_t status = read_status(NULL);
    assert(status.request_id == assign.request_id && status.result == NRF_UPSTREAM_RESULT_BUSY);
    assert(model_count == 4U);
    run_submitted_work();
    model_count = 0U;
    status = read_status(NULL);
    assert(status.request_id == queued_ids[3] && status.result == NRF_UPSTREAM_RESULT_OK);
    assert(model_count == 0U);
}

/* fail-before(real): a full CS execute queue must retain a request-correlated BUSY result after queued work drains. */
static void test_cs_execute_overflow_publishes_busy(void)
{
    nrf_cs_control_request_t requests[3];
    for (size_t index = 0U; index < 3U; ++index) {
        requests[index] = model_cs_request(0xA00U + (uint32_t)index, 0xA10U + (uint32_t)index);
        send_cs_execute_request(&requests[index]);
    }
    run_submitted_work();
    uint8_t frame[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    assert(nrf_cs_control_encode_status_request(&requests[2], frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
    const nrf_cs_control_result_t result = read_cs_result();
    assert(result.request_id == requests[2].request_id);
    assert(result.command_result == NRF_CS_CONTROL_RESULT_BUSY);
}

/* fail-before(real): submit failure removes only its item; a later successful submission must not run it. */
static void test_submit_failure_does_not_leak_queued_work(void)
{
    const unsigned before_sensor = s_sensor_assignment_calls;
    nrf_upstream_assign_t assign = make_assign(0xB00U);
    s_next_submit_failure = -ENOMEM;
    s_fail_next_submit = true;
    send_assign(&assign);
    nrf_upstream_status_t status = read_status(NULL);
    assert(status.request_id == assign.request_id && status.result == NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
    assert(s_submitted_work == NULL);

    assign.request_id++;
    send_assign(&assign);
    run_submitted_work();
    assert(s_sensor_assignment_calls == before_sensor + 1U);

    nrf_cs_control_request_t failed = model_cs_request(0xB10U, 0xB11U);
    nrf_cs_control_request_t later = model_cs_request(0xB12U, 0xB13U);
    s_next_submit_failure = -ENOMEM;
    s_fail_next_submit = true;
    send_cs_execute_request(&failed);
    assert(s_submitted_work == NULL);
    send_cs_execute_request(&later);
    run_submitted_work();
    uint8_t frame[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    assert(nrf_cs_control_encode_status_request(&failed, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
    const nrf_cs_control_result_t result = read_cs_result();
    assert(result.request_id == failed.request_id);
    assert(result.command_result == NRF_CS_CONTROL_RESULT_INTERNAL_ERROR);
}

/* characterization: malformed ASSIGNs and zero-length probes do not mutate the accepted assignment. */
static void test_assign_validation_and_empty_probe(void)
{
    uint32_t before_length = 0U;
    uint8_t before[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    uint8_t *frame = read_frame(&before_length);
    assert(before_length == sizeof(before));
    memcpy(before, frame, sizeof(before));
    s_callbacks->buf_write_received(NULL, NULL, 0U);
    frame = read_frame(&before_length);
    assert(before_length == sizeof(before) && memcmp(before, frame, sizeof(before)) == 0);

    uint8_t short_frame[3] = {0x47U, 0x53U, 1U};
    s_callbacks->buf_write_received(NULL, short_frame, sizeof(short_frame));
    nrf_upstream_status_t status = read_status(NULL);
    assert(status.result == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    nrf_upstream_assign_t assign = make_assign(0xC20U);
    uint8_t encoded[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
    assert(nrf_upstream_protocol_encode_assign(&assign, encoded) == NRF_UPSTREAM_RESULT_OK);
    encoded[22] ^= 1U;
    s_callbacks->buf_write_received(NULL, encoded, sizeof(encoded));
    status = read_status(NULL);
    assert(status.request_id == assign.request_id && status.result == NRF_UPSTREAM_RESULT_BAD_CRC);

    assert(nrf_upstream_protocol_encode_assign(&assign, encoded) == NRF_UPSTREAM_RESULT_OK);
    encoded[19] = 0x7FU;
    refresh_assign_crc(encoded);
    s_callbacks->buf_write_received(NULL, encoded, sizeof(encoded));
    status = read_status(NULL);
    assert(status.request_id == assign.request_id && status.result == NRF_UPSTREAM_RESULT_ROLE_INVALID);

    assign.request_id++;
    assert(nrf_upstream_protocol_encode_assign(&assign, encoded) == NRF_UPSTREAM_RESULT_OK);
    encoded[20] = 0xFFU;
    refresh_assign_crc(encoded);
    s_callbacks->buf_write_received(NULL, encoded, sizeof(encoded));
    status = read_status(NULL);
    assert(status.request_id == assign.request_id && status.result == NRF_UPSTREAM_RESULT_NODE_ID_INVALID);
}

/* characterization: a changed post-assignment role is rejected while prior queued idempotent work still drains. */
static void test_changed_assignment_conflict_preserves_queued_item(void)
{
    const unsigned before_sensor = s_sensor_assignment_calls;
    nrf_upstream_assign_t same = make_assign(0xC30U);
    nrf_upstream_assign_t changed = same;
    changed.request_id++;
    changed.role = NRF_UPSTREAM_ROLE_MAIN;
    changed.mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    send_assign(&same);
    send_assign(&changed);
    run_submitted_work();
    const nrf_upstream_status_t status = read_status(NULL);
    assert(status.request_id == changed.request_id);
    assert(status.result == NRF_UPSTREAM_RESULT_ROLE_CONFLICT);
    assert(status.role == NRF_UPSTREAM_ROLE_REMOTE && status.node_id == 2U);
    assert(s_sensor_assignment_calls == before_sensor + 1U);
}

/* characterization: startup purges TWIS work left pending across a simulated process restart. */
static void test_restart_purges_queued_assignment(void)
{
    /* The runtime keeps this pointer until process exit. */
    static nrf_node_state_t node;
    nrf_upstream_assign_t assign = make_assign(0xD00U);
    send_assign(&assign);
    assert(s_submitted_work != NULL);
    s_submitted_work = NULL;
    nrf_bootstrap_runtime_zephyr_test_forget_started();
    nrf_node_state_init(&node);
    assert(nrf_node_state_mark_ready(&node) == NRF_NODE_STATE_OK);
    assert(nrf_bootstrap_runtime_zephyr_start(&node) == 0);
    assert(s_submitted_work == NULL);
    const nrf_upstream_status_t status = read_status(NULL);
    assert(status.request_id == 0U && status.role == NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(status.result == NRF_UPSTREAM_RESULT_OK);
}

/* fail-before(real): run the full existing callback suite, then replay overflow, submit-failure, and restart models. */
int main(void)
{
    assert(max494_existing_runtime_test_main() == 0);
    test_assign_queue_model_and_request_correlation();
    test_cs_execute_overflow_publishes_busy();
    test_submit_failure_does_not_leak_queued_work();
    test_assign_validation_and_empty_probe();
    test_changed_assignment_conflict_preserves_queued_item();
    test_restart_purges_queued_assignment();
    return 0;
}
