#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"
#include "nrf_upstream_target_zephyr.h"
#include "nrf_watchdog_zephyr.h"

void nrf_cs_pair_runtime_zephyr_stub_configure(int execute_result, nrf_cs_outcome_t outcome);

static const struct i2c_target_callbacks *s_callbacks;
static struct k_work *s_submitted_work;
static unsigned s_submit_calls;
static unsigned s_readdress_calls;
static unsigned s_reboot_calls;
static unsigned s_sensor_assignment_calls;
static unsigned s_sensor_refresh_calls;
static unsigned s_height_reference_calls;
static unsigned s_imu_calibration_calls;
static bool s_registered;
static uint16_t s_address;
static int s_readdress_result;
static int s_sensor_assignment_result;
static bool s_readdress_rollback_discovery;
static nrf_upstream_sensor_snapshot_t s_sensor_snapshot;
static nrf_upstream_sensor_refresh_t s_last_sensor_refresh;
static nrf_upstream_height_reference_t s_last_height_reference;
static nrf_upstream_imu_calibrate_t s_last_imu_calibration;
static uint32_t s_cycle;
static int64_t s_uptime = 100U;

#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
static uint8_t s_interleave_request_frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
static size_t s_interleave_request_length;
static uint8_t s_interleave_response_frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
static uint32_t s_interleave_response_length;
static bool s_interleave_armed;
static bool s_interleave_fired;
static bool s_interleave_on_unlock;
#endif

int64_t k_uptime_get(void)
{
    return s_uptime++;
}

uint32_t k_cycle_get_32(void)
{
    return ++s_cycle;
}

int sys_csrand_get(void *destination, size_t length)
{
    const uint32_t boot_id = 0xC0FFEE42U;
    assert(length == sizeof(boot_id));
    memcpy(destination, &boot_id, sizeof(boot_id));
    return 0;
}

ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    static const uint8_t id[] = {0x10U, 0x20U, 0x30U, 0x40U};
    size_t copied = length < sizeof(id) ? length : sizeof(id);

    memcpy(buffer, id, copied);
    return (ssize_t)copied;
}

uint32_t nrf_watchdog_zephyr_boot_reset_cause(void)
{
    return 0x20U;
}

int k_work_submit(struct k_work *work)
{
    s_submitted_work = work;
    ++s_submit_calls;
    return 1;
}

void sys_reboot(int type)
{
    assert(type == 0);
    ++s_reboot_calls;
}

int nrf_sensor_upstream_runtime_zephyr_notify_remote_assignment(
    const nrf_upstream_assign_t *assign)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;
    nrf_upstream_status_t status;

    assert(assign != NULL);
    assert(assign->role == NRF_UPSTREAM_ROLE_REMOTE);
    assert(s_callbacks != NULL);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    assert(length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH ||
           length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
    assert(nrf_upstream_protocol_decode_status(
               frame,
               NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH,
               &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.request_id == assign->request_id);
    assert(status.result == NRF_UPSTREAM_RESULT_PENDING);
    ++s_sensor_assignment_calls;
    return s_sensor_assignment_result;
}

int nrf_sensor_upstream_runtime_zephyr_request_refresh(
    const nrf_upstream_sensor_refresh_t *request)
{
    assert(request != NULL);
    s_last_sensor_refresh = *request;
    ++s_sensor_refresh_calls;
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_request_height_reference(
    const nrf_upstream_height_reference_t *request)
{
    assert(request != NULL);
    s_last_height_reference = *request;
    ++s_height_reference_calls;
    return 0;
}

int nrf_sensor_upstream_runtime_zephyr_request_imu_calibration(
    const nrf_upstream_imu_calibrate_t *request)
{
    assert(request != NULL);
    s_last_imu_calibration = *request;
    ++s_imu_calibration_calls;
    return 0;
}

bool nrf_sensor_upstream_runtime_zephyr_get_snapshot(
    nrf_upstream_sensor_snapshot_t *snapshot)
{
    assert(snapshot != NULL);
    *snapshot = s_sensor_snapshot;
    return true;
}

int nrf_upstream_target_zephyr_read_device_id(uint8_t *buffer, size_t capacity, size_t *out_length)
{
    static const uint8_t id[] = {0x10U, 0x20U, 0x30U, 0x40U};
    assert(capacity >= sizeof(id));
    memcpy(buffer, id, sizeof(id));
    *out_length = sizeof(id);
    return 0;
}

int nrf_upstream_target_zephyr_init(const struct i2c_target_callbacks *callbacks)
{
    s_callbacks = callbacks;
    return 0;
}

int nrf_upstream_target_zephyr_register(uint16_t address)
{
    s_registered = true;
    s_address = address;
    return 0;
}

int nrf_upstream_target_zephyr_unregister(void)
{
    s_registered = false;
    s_address = 0U;
    return 0;
}

int nrf_upstream_target_zephyr_readdress(uint16_t address)
{
    ++s_readdress_calls;
    if (s_readdress_result == 0) {
        s_registered = true;
        s_address = address;
        return 0;
    }
    if (s_readdress_rollback_discovery) {
        s_registered = true;
        s_address = NRF_UPSTREAM_ADDRESS_DISCOVERY;
    }
    else {
        s_registered = false;
        s_address = 0U;
    }
    return s_readdress_result;
}

bool nrf_upstream_target_zephyr_is_registered(void)
{
    return s_registered;
}

uint16_t nrf_upstream_target_zephyr_address(void)
{
    return s_registered ? s_address : 0U;
}

static nrf_upstream_assign_t make_assign(uint32_t request_id)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = request_id;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 0x10U;
    assign.hardware_id[1] = 0x20U;
    assign.hardware_id[2] = 0x30U;
    assign.hardware_id[3] = 0x40U;
    assign.role = NRF_UPSTREAM_ROLE_REMOTE;
    assign.node_id = 2U;
    assign.mux_channel = 1U;
    return assign;
}

static uint8_t *read_frame(uint32_t *read_length)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;
    assert(s_callbacks != NULL);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    if (read_length != NULL) {
        *read_length = length;
    }
    return frame;
}

static nrf_upstream_status_t read_status(uint32_t *read_length)
{
    uint32_t length;
    uint8_t *frame = read_frame(&length);
    nrf_upstream_status_t status;

    if (read_length != NULL) {
        *read_length = length;
    }
    assert(length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH ||
           length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    assert(nrf_upstream_protocol_decode_status(
               frame,
               NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH,
               &status) == NRF_UPSTREAM_RESULT_OK);
    return status;
}

static void send_log_status(uint32_t request_id)
{
    nrf_log_status_request_t request = {.request_id = request_id};
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    assert(nrf_log_protocol_encode_status_request(&request, frame) == NRF_LOG_ERROR_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void send_log_read(const nrf_log_read_request_t *request)
{
    uint8_t frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    assert(nrf_log_protocol_encode_read_request(request, frame) == NRF_LOG_ERROR_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static nrf_log_status_t read_log_status(void);

#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
static void capture_interleaved_twis_response(void)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;

    s_interleave_armed = false;
    s_interleave_fired = true;
    assert(s_callbacks != NULL);
    s_callbacks->buf_write_received(
        NULL,
        s_interleave_request_frame,
        (uint32_t)s_interleave_request_length);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    assert(length <= sizeof(s_interleave_response_frame));
    memcpy(s_interleave_response_frame, frame, length);
    s_interleave_response_length = length;
}

void nrf_bootstrap_runtime_test_spinlock_hook(struct k_spinlock *lock)
{
    (void)lock;
    if (s_interleave_armed && !s_interleave_on_unlock) {
        capture_interleaved_twis_response();
    }
}

void nrf_bootstrap_runtime_test_spinunlock_hook(struct k_spinlock *lock)
{
    (void)lock;
    if (s_interleave_armed && s_interleave_on_unlock) {
        capture_interleaved_twis_response();
    }
}

static void test_twis_log_response_interleaves_application_append(void)
{
    static const uint32_t old_values[] = {0x30800001U, 0x30800002U, 0x30800003U};
    static const uint32_t start_sequences[] = {700U, 900U};
    static const uint32_t request_ids[] = {0x308100U, 0x308200U};
    size_t kind;

    for (kind = 0U; kind < 2U; ++kind) {
        unsigned int point;
        for (point = 0U; point < 2U; ++point) {
            const bool on_unlock = point != 0U;
            nrf_log_status_t before;
            nrf_log_status_t after;
            uint8_t *retry_frame;
            uint32_t retry_length = 0U;
            uint32_t app_value = 0x3080A000U + (uint32_t)kind;
            uint32_t later_value = 0x3080B000U + (uint32_t)kind;
            uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
            size_t frame_length = 0U;
            nrf_log_read_request_t latest_request;
            nrf_log_block_t latest_block;
            const size_t old_value_count = sizeof(old_values) / sizeof(old_values[0]);

            assert(nrf_log_runtime_zephyr_test_set_next_seq(start_sequences[kind]) ==
                   NRF_LOG_ERROR_OK);
            for (size_t index = 0U; index < old_value_count; ++index) {
                nrf_log_runtime_zephyr_append_event(
                    NRF_LOG_EVENT_LOG_TEST_EVENT,
                    old_values[index],
                    ~old_values[index]);
            }
            before = read_log_status();
            assert(before.available == old_value_count);
            assert(before.next_seq - before.oldest_seq == before.available);

            if (kind == 0U) {
                const nrf_log_status_request_t request = {.request_id = request_ids[kind]};
                assert(nrf_log_protocol_encode_status_request(
                           &request,
                           s_interleave_request_frame) == NRF_LOG_ERROR_OK);
                s_interleave_request_length = NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH;
            } else {
                const nrf_log_read_request_t request = {
                    .request_id = request_ids[kind],
                    .expected_boot_id = before.boot_id,
                    .cursor_seq = before.oldest_seq,
                    .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
                };
                assert(nrf_log_protocol_encode_read_request(
                           &request,
                           s_interleave_request_frame) == NRF_LOG_ERROR_OK);
                s_interleave_request_length = NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH;
            }

            s_interleave_response_length = 0U;
            s_interleave_fired = false;
            s_interleave_on_unlock = on_unlock;
            s_interleave_armed = true;
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_LOG_TEST_EVENT,
                app_value,
                ~app_value);
            assert(!s_interleave_armed && s_interleave_fired);
            assert(s_interleave_response_length != 0U);
            nrf_log_runtime_zephyr_append_event(
                NRF_LOG_EVENT_LOG_TEST_EVENT,
                later_value,
                ~later_value);

            /* The hooks model the read winning acquire, or running after append release. */
            s_callbacks->buf_write_received(
                NULL,
                s_interleave_request_frame,
                (uint32_t)s_interleave_request_length);
            retry_frame = read_frame(&retry_length);
            assert(retry_length == s_interleave_response_length);
            assert(memcmp(
                       retry_frame,
                       s_interleave_response_frame,
                       retry_length) == 0);

            if (kind == 0U) {
                nrf_log_status_t response;
                const uint32_t visible_appends = on_unlock ? 1U : 0U;
                assert(s_interleave_response_length == NRF_LOG_PROTOCOL_STATUS_LENGTH);
                assert(nrf_log_protocol_decode_status(
                           s_interleave_response_frame,
                           s_interleave_response_length,
                           &response) == NRF_LOG_ERROR_OK);
                assert(response.request_id == request_ids[kind]);
                assert(response.boot_id == before.boot_id);
                assert(response.oldest_seq == before.oldest_seq);
                assert(response.next_seq == before.next_seq + visible_appends);
                assert(response.available == before.available + visible_appends);
                assert(response.overwritten_total == before.overwritten_total);
                assert(response.lost_internal_total == before.lost_internal_total);
                assert(response.next_seq - response.oldest_seq == response.available);
            } else {
                nrf_log_block_validation_t validation = {0};
                nrf_log_block_t response;
                const uint32_t visible_appends = on_unlock ? 1U : 0U;
                validation.has_expected_boot_id = true;
                validation.expected_boot_id = before.boot_id;
                validation.has_cursor = true;
                validation.cursor_seq = before.oldest_seq;
                validation.has_max_records = true;
                validation.max_records = NRF_LOG_PROTOCOL_MAX_RECORDS;
                validation.has_status_next_seq = true;
                validation.status_next_seq = before.next_seq + visible_appends;
                assert(nrf_log_protocol_decode_block(
                           s_interleave_response_frame,
                           s_interleave_response_length,
                           &validation,
                           &response) == NRF_LOG_ERROR_OK);
                assert(response.record_count == old_value_count + visible_appends);
                assert(response.first_seq == before.oldest_seq);
                assert(response.next_cursor == before.next_seq + visible_appends);
                assert(response.oldest_seq == before.oldest_seq);
                assert(response.overwritten_total == before.overwritten_total);
                for (size_t index = 0U; index < old_value_count; ++index) {
                    assert(response.records[index].seq == before.oldest_seq + index);
                    assert(response.records[index].event_id == NRF_LOG_EVENT_LOG_TEST_EVENT);
                    assert(response.records[index].arg0 == old_values[index]);
                    assert(response.records[index].arg1 == ~response.records[index].arg0);
                }
                if (on_unlock) {
                    const size_t app_index = old_value_count;
                    assert(response.records[app_index].seq == before.next_seq);
                    assert(response.records[app_index].arg0 == app_value);
                    assert(response.records[app_index].arg1 == ~app_value);
                }
            }

            after = read_log_status();
            assert(after.boot_id == before.boot_id);
            assert(after.next_seq == before.next_seq + 2U);
            assert(after.available == before.available + 2U);
            assert(after.oldest_seq == before.oldest_seq);
            assert(after.overwritten_total == before.overwritten_total);
            assert(after.lost_internal_total == before.lost_internal_total);
            assert(after.next_seq - after.oldest_seq == after.available);

            latest_request.request_id = request_ids[kind] + 1U;
            latest_request.expected_boot_id = before.boot_id;
            latest_request.cursor_seq = before.next_seq + 1U;
            latest_request.max_records = 1U;
            assert(nrf_log_runtime_zephyr_build_block(
                       &latest_request,
                       frame,
                       sizeof(frame),
                       &frame_length) == NRF_LOG_ERROR_OK);
            assert(nrf_log_protocol_decode_block(
                       frame,
                       frame_length,
                       NULL,
                       &latest_block) == NRF_LOG_ERROR_OK);
            assert(latest_block.record_count == 1U);
            assert(latest_block.records[0].seq == before.next_seq + 1U);
            assert(latest_block.records[0].arg0 == later_value);
            assert(latest_block.records[0].arg1 == ~later_value);

        }
    }
}
#endif

static nrf_log_status_t read_log_status(void)
{
    nrf_log_status_t status;
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    size_t length = 0U;
    assert(nrf_log_runtime_zephyr_build_status(
               0U,
               frame,
               sizeof(frame),
               &length) == NRF_LOG_ERROR_OK);
    assert(nrf_log_protocol_decode_status(frame, length, &status) == NRF_LOG_ERROR_OK);
    return status;
}

static nrf_log_record_t find_log_event(uint16_t event_id)
{
    const nrf_log_status_t status = read_log_status();
    uint32_t cursor = status.oldest_seq;
    uint32_t request_id = 0xE0000000U;
    nrf_log_record_t found = {0};
    bool found_valid = false;

    while (cursor < status.next_seq) {
        nrf_log_read_request_t request = {
            .request_id = request_id++,
            .expected_boot_id = status.boot_id,
            .cursor_seq = cursor,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        nrf_log_block_t block;
        uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        size_t length = 0U;
        assert(nrf_log_runtime_zephyr_build_block(
                   &request,
                   frame,
                   sizeof(frame),
                   &length) == NRF_LOG_ERROR_OK);
        assert(nrf_log_protocol_decode_block(frame, length, NULL, &block) == NRF_LOG_ERROR_OK);
        for (uint8_t index = 0U; index < block.record_count; ++index) {
            if (block.records[index].event_id == event_id) {
                found = block.records[index];
                found_valid = true;
            }
        }
        assert(block.next_cursor > cursor);
        cursor = block.next_cursor;
    }
    assert(found_valid && "event was not recorded");
    return found;
}

static void send_assign(const nrf_upstream_assign_t *assign)
{
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
    assert(nrf_upstream_protocol_encode_assign(assign, frame) == NRF_UPSTREAM_RESULT_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void send_sensor_refresh(uint32_t request_id)
{
    nrf_upstream_sensor_refresh_t request = {.request_id = request_id};
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH];
    assert(nrf_upstream_protocol_encode_sensor_refresh(&request, frame) == NRF_UPSTREAM_RESULT_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void send_height_reference(
    uint8_t action,
    uint32_t request_id,
    uint16_t generation)
{
    nrf_upstream_height_reference_t request = {
        .action = action,
        .request_id = request_id,
        .generation = generation,
    };
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH];

    assert(nrf_upstream_protocol_encode_height_reference(&request, frame) ==
           NRF_UPSTREAM_RESULT_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void send_imu_calibration(uint8_t action, uint32_t request_id, uint16_t generation)
{
    nrf_upstream_imu_calibrate_t request = {
        .action = action,
        .request_id = request_id,
        .generation = generation,
    };
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH];
    assert(nrf_upstream_protocol_encode_imu_calibrate(&request, frame) ==
           NRF_UPSTREAM_RESULT_OK);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void run_submitted_work(void)
{
    struct k_work *work = s_submitted_work;
    assert(work != NULL && work->handler != NULL);
    s_submitted_work = NULL;
    work->handler(work);
}

static void check_cs_failure_event(
    uint32_t request_id,
    uint32_t attempt_id,
    nrf_cs_outcome_t outcome,
    int execute_result)
{
    nrf_cs_control_request_t request = {0};
    nrf_cs_control_result_t result;
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
    uint32_t length;
    uint8_t *response;

    request.request_id = request_id;
    request.step.cycle_id = 1U;
    request.step.membership_generation = 1U;
    request.step.attempt_id = attempt_id;
    request.step.pair_index = 0U;
    request.step.attempt_number = 1U;
    request.step.initiator = 2U;
    request.step.reflector = 3U;
    request.step.timeout_ms = 500U;
    nrf_cs_pair_runtime_zephyr_stub_configure(execute_result, outcome);
    assert(nrf_cs_control_encode_execute(&request, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
    run_submitted_work();

    assert(nrf_cs_control_encode_status_request(&request, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, NRF_CS_CONTROL_STATUS_REQUEST_LENGTH);
    response = read_frame(&length);
    assert(length == NRF_CS_CONTROL_RESULT_LENGTH);
    assert(nrf_cs_control_decode_result(response, length, &result) == 0);
    assert(result.request_id == request_id);
    assert(result.state == NRF_CS_CONTROL_STATE_FAILED);
    assert(result.measurement.outcome == (execute_result == 0
        ? outcome
        : NRF_CS_OUTCOME_TRANSPORT_ERROR));

    if (execute_result != 0) {
        assert(find_log_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED).arg0 == attempt_id);
        assert(find_log_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED).arg1 == (uint32_t)execute_result);
    } else if (outcome == NRF_CS_OUTCOME_TIMEOUT) {
        const nrf_log_record_t event = find_log_event(NRF_LOG_EVENT_CS_TIMEOUT);
        assert(event.arg0 == attempt_id && event.arg1 == request.step.timeout_ms);
    } else {
        const nrf_log_record_t event = find_log_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED);
        assert(event.arg0 == attempt_id && event.arg1 == (uint32_t)outcome);
    }
}

int main(void)
{
    nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_upstream_assign_t assign;
    unsigned i;
    uint32_t read_length;

    memset(&s_sensor_snapshot, 0, sizeof(s_sensor_snapshot));
    s_sensor_snapshot.result = NRF_UPSTREAM_SENSOR_RESULT_NOT_READY;

    assert(nrf_log_runtime_zephyr_init() == 0);
    nrf_node_state_init(&node_state);
    assert(nrf_node_state_mark_ready(&node_state) == NRF_NODE_STATE_OK);
    s_readdress_result = 0;
    s_sensor_assignment_result = 0;
    s_readdress_rollback_discovery = true;
    assert(nrf_bootstrap_runtime_zephyr_start(&node_state) == 0);
    assert(s_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    status = read_status(&read_length);
    assert(read_length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
    assert(status.request_id == 0U && status.role == NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    assert(status.flags == NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED);

    assign = make_assign(10U);
    s_readdress_result = -5;
    send_assign(&assign);
    assert(s_readdress_calls == 0U);
    assert(s_submit_calls == 1U);
    status = read_status(NULL);
    assert(status.request_id == 0U);
    run_submitted_work();
    assert(s_readdress_calls == 1U);
    assert(s_sensor_assignment_calls == 0U);
    status = read_status(NULL);
    assert(status.request_id == 10U && status.result == NRF_UPSTREAM_RESULT_READDRESS_FAILED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    s_readdress_result = 0;
    send_assign(&assign);
    assert(s_readdress_calls == 1U);
    run_submitted_work();
    assert(s_readdress_calls == 2U);
    assert(s_sensor_assignment_calls == 1U);
    status = read_status(&read_length);
    assert(read_length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    assert(status.request_id == 10U && status.result == NRF_UPSTREAM_RESULT_OK);
    assert(status.role == NRF_UPSTREAM_ROLE_REMOTE && status.node_id == 2U && status.mux_channel == 1U);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_REMOTE);
    {
        const nrf_log_record_t assigned = find_log_event(NRF_LOG_EVENT_ROLE_ASSIGNED);
        const nrf_log_status_t logs = read_log_status();
        assert(logs.next_seq == 2U);
        assert(assigned.arg0 == 2U && assigned.arg1 == NRF_UPSTREAM_ROLE_REMOTE);
    }

    {
        unsigned before_submit = s_submit_calls;
        nrf_upstream_status_t before = status;
        send_sensor_refresh(0x12345678U);
        assert(s_sensor_refresh_calls == 1U);
        assert(s_last_sensor_refresh.request_id == 0x12345678U);
        assert(s_submit_calls == before_submit);
        status = read_status(NULL);
        assert(memcmp(&status, &before, sizeof(status)) == 0);
    }

    {
        const unsigned before_submit = s_submit_calls;
        const unsigned before_refresh = s_sensor_refresh_calls;
        send_height_reference(
            NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE,
            0x87654321U,
            UINT16_MAX);
        assert(s_height_reference_calls == 1U);
        assert(s_last_height_reference.action == NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE);
        assert(s_last_height_reference.request_id == 0x87654321U);
        assert(s_last_height_reference.generation == UINT16_MAX);
        assert(s_submit_calls == before_submit);
        assert(s_sensor_refresh_calls == before_refresh);
    }

    {
        const unsigned before_submit = s_submit_calls;
        const unsigned before_refresh = s_sensor_refresh_calls;
        send_imu_calibration(
            NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE,
            0x10203040U,
            UINT16_MAX);
        assert(s_imu_calibration_calls == 1U);
        assert(s_last_imu_calibration.action == NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE);
        assert(s_last_imu_calibration.request_id == 0x10203040U);
        assert(s_last_imu_calibration.generation == UINT16_MAX);
        assert(s_submit_calls == before_submit);
        assert(s_sensor_refresh_calls == before_refresh);

        send_imu_calibration(
            NRF_UPSTREAM_IMU_CALIBRATE_CLEAR,
            0x10203041U,
            0U);
        assert(s_imu_calibration_calls == 2U);
        assert(s_last_imu_calibration.action == NRF_UPSTREAM_IMU_CALIBRATE_CLEAR);
        assert(s_last_imu_calibration.request_id == 0x10203041U);
        assert(s_last_imu_calibration.generation == 0U);
        assert(s_submit_calls == before_submit);
        assert(s_sensor_refresh_calls == before_refresh);
    }

    /*
     * REMOTE assignment confirmation must not publish OK before sensor logical
     * activation/ACTIVATE enqueue succeeds. A post-commit enqueue failure is
     * not safely rollbackable at the bootstrap layer, so fail closed by
     * publishing INTERNAL_ERROR and requesting a cold reboot.
     */
    s_sensor_assignment_result = -ENOSPC;
    assign.request_id = 11U;
    send_assign(&assign);
    run_submitted_work();
    assert(s_sensor_assignment_calls == 2U);
    assert(s_reboot_calls == 1U);
    status = read_status(NULL);
    assert(status.request_id == 11U);
    assert(status.result == NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
    assert(status.role == NRF_UPSTREAM_ROLE_REMOTE);

    /* The host reboot stub returns; prove a later retry can publish OK again. */
    s_sensor_assignment_result = 0;
    assign.request_id = 12U;
    send_assign(&assign);
    run_submitted_work();
    assert(s_sensor_assignment_calls == 3U);
    status = read_status(NULL);
    assert(status.request_id == 12U && status.result == NRF_UPSTREAM_RESULT_OK);
    assert(read_log_status().next_seq == 2U);

    {
        const unsigned before_sensor_assignment_calls = s_sensor_assignment_calls;
        for (i = 0U; i < 4U; ++i) {
            assign.request_id = 20U + i;
            send_assign(&assign);
        }
        assign.request_id = 24U;
        send_assign(&assign);
        status = read_status(NULL);
        assert(status.request_id == 24U && status.result == NRF_UPSTREAM_RESULT_BUSY);
        run_submitted_work();
        status = read_status(NULL);
        assert(status.request_id == 23U && status.result == NRF_UPSTREAM_RESULT_OK);
        assert(s_readdress_calls == 2U);
        assert(s_sensor_assignment_calls == before_sensor_assignment_calls + 4U);
    }

    {
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
        unsigned before = s_submit_calls;
        assign = make_assign(30U);
        assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
        frame[20] ^= 1U;
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        assert(s_submit_calls == before);
        status = read_status(NULL);
        assert(status.request_id == 30U && status.result == NRF_UPSTREAM_RESULT_BAD_CRC);
    }

    {
        nrf_upstream_sensor_refresh_t request = {.request_id = 99U};
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH];
        unsigned before = s_sensor_refresh_calls;
        status = read_status(NULL);
        assert(nrf_upstream_protocol_encode_sensor_refresh(&request, frame) == NRF_UPSTREAM_RESULT_OK);
        frame[6] ^= 1U;
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        assert(s_sensor_refresh_calls == before);
        {
            nrf_upstream_status_t after = read_status(NULL);
            assert(memcmp(&status, &after, sizeof(status)) == 0);
        }
    }

    assert(find_log_event(NRF_LOG_EVENT_TWIS_BUSY).arg0 == NRF_UPSTREAM_MESSAGE_ASSIGN);
    {
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];
        uint16_t crc;
        assign = make_assign(70U);
        assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
        frame[3] = 0x55U;
        crc = nrf_upstream_protocol_crc16_ccitt_false(frame, sizeof(frame) - 2U);
        frame[sizeof(frame) - 2U] = (uint8_t)crc;
        frame[sizeof(frame) - 1U] = (uint8_t)(crc >> 8U);
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        status = read_status(NULL);
        assert(status.request_id == 70U && status.result == NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE);
        assert(status.role == NRF_UPSTREAM_ROLE_REMOTE && status.node_id == 2U);
        assert(find_log_event(NRF_LOG_EVENT_TWIS_UNKNOWN_TYPE).arg0 == 0x55U);
    }

    {
        const nrf_log_status_t before = read_log_status();
        nrf_log_read_request_t request = {
            .request_id = 0x1234U,
            .expected_boot_id = before.boot_id,
            .cursor_seq = before.next_seq,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        uint8_t first_response[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        uint8_t repeated_response[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        uint8_t changed_response[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        size_t first_length;
        uint32_t repeated_length;
        uint32_t changed_length;
        nrf_log_block_t block;
        uint8_t *response_frame;

        send_log_read(&request);
        response_frame = read_frame(&repeated_length);
        memcpy(first_response, response_frame, repeated_length);
        first_length = repeated_length;
        assert(nrf_log_protocol_decode_block(
                   first_response,
                   first_length,
                   NULL,
                   &block) == NRF_LOG_ERROR_OK);
        assert(block.record_count == 0U);

        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_LOG_TEST_EVENT, 3U, 4U);
        send_log_read(&request);
        response_frame = read_frame(&repeated_length);
        memcpy(repeated_response, response_frame, repeated_length);
        assert(repeated_length == first_length);
        assert(memcmp(first_response, repeated_response, first_length) == 0);

        ++request.request_id;
        send_log_read(&request);
        response_frame = read_frame(&changed_length);
        memcpy(changed_response, response_frame, changed_length);
        assert(changed_length != 0U && changed_length != repeated_length);
        assert(nrf_log_protocol_decode_block(
                   changed_response,
                   changed_length,
                   NULL,
                   &block) == NRF_LOG_ERROR_OK);
        assert(block.request_id == request.request_id);
        assert(block.record_count == 1U);
        assert(block.records[0].event_id == NRF_LOG_EVENT_LOG_TEST_EVENT);
    }

    {
        nrf_cs_control_request_t request = {0};
        uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];
        nrf_cs_control_result_t cs_result;
        nrf_log_status_request_t log_status_request = {.request_id = 0x5678U};
        uint8_t log_request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
        uint8_t first_read[NRF_LOG_PROTOCOL_STATUS_LENGTH];
        uint8_t second_read[NRF_LOG_PROTOCOL_STATUS_LENGTH];
        uint32_t first_length;
        uint32_t second_length;
        uint8_t *response_frame;

        request.request_id = 0x501U;
        request.step.cycle_id = 1U;
        request.step.membership_generation = 1U;
        request.step.attempt_id = 0x502U;
        request.step.pair_index = 0U;
        request.step.attempt_number = 1U;
        request.step.initiator = 2U;
        request.step.reflector = 3U;
        request.step.timeout_ms = 500U;
        assert(nrf_cs_control_encode_execute(&request, frame) == 0);
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        run_submitted_work();

        assert(nrf_cs_control_encode_status_request(&request, frame) == 0);
        s_callbacks->buf_write_received(NULL, frame, NRF_CS_CONTROL_STATUS_REQUEST_LENGTH);
        assert(nrf_log_protocol_encode_status_request(
                   &log_status_request,
                   log_request_frame) == NRF_LOG_ERROR_OK);
        s_callbacks->buf_write_received(NULL, log_request_frame, sizeof(log_request_frame));
        response_frame = read_frame(&first_length);
        memcpy(first_read, response_frame, first_length);
        response_frame = read_frame(&second_length);
        memcpy(second_read, response_frame, second_length);
        assert(first_length == NRF_LOG_PROTOCOL_STATUS_LENGTH);
        assert(second_length == first_length);
        assert(memcmp(first_read, second_read, first_length) == 0);
        assert(nrf_log_protocol_decode_status(
                   first_read,
                   first_length,
                   &(nrf_log_status_t){0}) == NRF_LOG_ERROR_OK);

        send_sensor_refresh(0x5679U);
        {
            uint8_t *cs_frame = read_frame(&first_length);
            assert(first_length == NRF_CS_CONTROL_RESULT_LENGTH);
            assert(nrf_cs_control_decode_result(cs_frame, first_length, &cs_result) == 0);
            assert(cs_result.request_id == request.request_id);
            assert(cs_result.state == NRF_CS_CONTROL_STATE_COMPLETE);
        }
        status = read_status(&read_length);
        assert(read_length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
        assert(status.role == NRF_UPSTREAM_ROLE_REMOTE);
    }

    check_cs_failure_event(
        0x601U,
        0x602U,
        NRF_CS_OUTCOME_TIMEOUT,
        0);
    check_cs_failure_event(
        0x603U,
        0x604U,
        NRF_CS_OUTCOME_TRANSPORT_ERROR,
        0);
    check_cs_failure_event(
        0x605U,
        0x606U,
        NRF_CS_OUTCOME_SUCCESS,
        -ENOTSUP);

    {
        nrf_upstream_status_t before = read_status(NULL);
        nrf_log_status_request_t request = {.request_id = 0xDEADU};
        uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
        nrf_upstream_status_t error_status;
        assert(nrf_log_protocol_encode_status_request(&request, frame) == NRF_LOG_ERROR_OK);
        frame[sizeof(frame) - 1U] ^= 1U;
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        error_status = read_status(&read_length);
        assert(read_length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
        assert(error_status.request_id == request.request_id);
        assert(error_status.result == NRF_UPSTREAM_RESULT_INVALID_FRAME);
        assert(find_log_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME).arg0 == NRF_LOG_MESSAGE_STATUS_REQUEST);

        send_sensor_refresh(0xBEEFU);
        status = read_status(NULL);
        assert(memcmp(&before, &status, sizeof(status)) == 0);
        assert(nrf_log_runtime_zephyr_max_block_build_cycles() != 0U);
    }

#if defined(NRF_BOOTSTRAP_SPINLOCK_INTERLEAVE_TEST)
    test_twis_log_response_interleaves_application_append();
#endif
    assert(s_reboot_calls == 1U);
    return 0;
}
