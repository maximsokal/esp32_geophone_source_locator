#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(NRF_TWIS_RUNTIME_INTEGRATION)
#include <nrfx_twis.h>
#endif
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#include "nrf_cs_cycle_runtime.h"
#include "nrf_boot_diagnostics.h"
#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_cs_control_protocol.h"
#include "nrf_log_events.h"
#include "nrf_log_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"

extern struct k_msgq s_cs_execute_queue;
extern unsigned nrf_cs_pair_zephyr_stub_execute_calls;
extern int nrf_cs_pair_zephyr_stub_set_node_result;
extern bool nrf_cs_pair_zephyr_stub_radio_enabled;
extern unsigned nrf_cs_pair_zephyr_stub_radio_changes;
#include "nrf_upstream_target_zephyr.h"
#include "nrf_watchdog_zephyr.h"
#include "nrf_reboot_intent_zephyr.h"
#include "nrf_hci_workqueue.h"

void nrf_cs_pair_runtime_zephyr_stub_configure(int execute_result, nrf_cs_outcome_t outcome);

static const struct i2c_target_callbacks *s_callbacks;

#if defined(NRF_TWIS_RUNTIME_INTEGRATION)
/* Real trace wrapper + real application callback; only SDK register/IRQ delivery is simulated. */
NRF_TWIS_Type test_upstream_registers;
static nrfx_twis_t s_test_twis = {&test_upstream_registers};
static nrfx_twis_event_handler_t s_test_twis_handler;
static uint8_t s_test_rx[16];
int __wrap_nrfx_twis_init(nrfx_twis_t *, const nrfx_twis_config_t *, nrfx_twis_event_handler_t);
int __wrap_nrfx_twis_rx_prepare(nrfx_twis_t *, void *, size_t);
void __wrap_nrfx_twis_irq_handler(nrfx_twis_t *);
int __real_nrfx_twis_init(nrfx_twis_t *instance, const nrfx_twis_config_t *config,
                         nrfx_twis_event_handler_t handler)
{
    (void)instance; (void)config;
    s_test_twis_handler = handler;
    return 0;
}
int __real_nrfx_twis_rx_prepare(nrfx_twis_t *instance, void *buffer, size_t length)
{
    (void)length;
    instance->p_reg->rx_buffer = buffer;
    return 0;
}
int __real_nrfx_twis_tx_prepare(nrfx_twis_t *instance, const void *buffer, size_t length)
{
    (void)instance; (void)buffer; (void)length;
    return 0;
}
void __real_nrfx_twis_irq_handler(nrfx_twis_t *instance)
{
    instance->p_reg->rx_started = false;
}
static void test_twis_shim(const nrfx_twis_event_t *event)
{
    if (event->type == NRFX_TWIS_EVT_WRITE_REQ) {
        assert(__wrap_nrfx_twis_rx_prepare(&s_test_twis, s_test_rx, sizeof(s_test_rx)) == 0);
    } else if (event->type == NRFX_TWIS_EVT_WRITE_DONE) {
        s_callbacks->buf_write_received(NULL, s_test_rx, (uint32_t)nrfx_twis_rx_amount(&s_test_twis));
    }
}

#endif
static struct k_work *s_submitted_work;
static struct k_work_delayable *s_delayed_work;
static unsigned s_submit_calls;
static bool s_fail_next_submit;
static int s_next_submit_failure;
static unsigned s_readdress_calls;
static unsigned s_reboot_calls;
static unsigned s_reboot_intent_count;
static nrf_reboot_intent_stage_t s_reboot_intent_stage;
static int s_reboot_intent_error;
static unsigned s_sensor_assignment_calls;
static uint32_t s_sensor_assignment_request_ids[128];
static nrf_upstream_assign_t s_sensor_assignments[128];
static unsigned s_work_queue_start_calls;
static unsigned s_sensor_refresh_calls;
static unsigned s_sensor_scan_calls;
static uint32_t s_last_sensor_scan_id;
static int s_sensor_scan_result;
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

struct k_work_q k_sys_work_q = { .started = true, .name = "system_workqueue" };

void k_work_queue_init(struct k_work_q *queue)
{
    memset(queue, 0, sizeof(*queue));
}

void k_work_queue_start(
    struct k_work_q *queue,
    unsigned char *stack,
    size_t stack_size,
    int priority,
    const struct k_work_queue_config *config)
{
    (void)stack;
    queue->started = true;
    queue->stack_size = stack_size;
    queue->priority = priority;
    queue->name = config != NULL ? config->name : NULL;
    ++s_work_queue_start_calls;
}

int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work)
{
    if (s_fail_next_submit) {
        s_fail_next_submit = false;
        ++s_submit_calls;
        return s_next_submit_failure;
    }
    ++s_submit_calls;
    if (queue == NULL || !queue->started || work == NULL) {
        return -EAGAIN;
    }
    if (work->queued) {
        return 0;
    }
    work->queue = queue;
    work->queued = true;
    s_submitted_work = work;
    return 1;
}

int k_work_submit(struct k_work *work)
{
    return k_work_submit_to_queue(&k_sys_work_q, work);
}

int k_work_reschedule_for_queue(
    struct k_work_q *queue,
    struct k_work_delayable *work,
    k_timeout_t delay)
{
    (void)delay;
    if (queue == NULL || !queue->started || work == NULL) {
        return -EAGAIN;
    }
    work->work.queue = queue;
    s_delayed_work = work;
    return 1;
}

void nrf_reboot_intent_zephyr_record(nrf_reboot_intent_stage_t stage, int error_code)
{
    ++s_reboot_intent_count;
    s_reboot_intent_stage = stage;
    s_reboot_intent_error = error_code;
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
    assert(s_sensor_assignment_calls < sizeof(s_sensor_assignment_request_ids) / sizeof(s_sensor_assignment_request_ids[0]));
    s_sensor_assignment_request_ids[s_sensor_assignment_calls] = assign->request_id;
    s_sensor_assignments[s_sensor_assignment_calls] = *assign;
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

int nrf_sensor_upstream_runtime_zephyr_request_scan(
    const nrf_upstream_sensor_refresh_t *request)
{
    assert(request != NULL);
    ++s_sensor_scan_calls;
    s_last_sensor_scan_id = request->request_id;
    return s_sensor_scan_result;
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

/*
 * What the ESP32 master reads: the target hands the TWIS one window per read, the master fetches the rest of
 * a longer response with READ_WINDOW requests. Returns the whole response.
 */
static uint8_t s_master_frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

static uint8_t *read_frame(uint32_t *read_length)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;
    assert(s_callbacks != NULL);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    const uint32_t total = nrf_bootstrap_runtime_zephyr_response_length();
    if (total > length) {
        const uint32_t step = NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
        assert(length == step && total <= sizeof(s_master_frame));
        memcpy(s_master_frame, frame, length);
        for (uint32_t offset = step; offset < total; offset += step) {
            const nrf_upstream_read_window_t window = {.offset = (uint16_t)offset};
            uint8_t request[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
            uint8_t *part = NULL;
            uint32_t part_length = 0U;
            assert(nrf_upstream_protocol_encode_read_window(&window, request) == NRF_UPSTREAM_RESULT_OK);
            s_callbacks->buf_write_received(NULL, request, sizeof(request));
            assert(s_callbacks->buf_read_requested(NULL, &part, &part_length) == 0);
            assert(part_length == (total - offset < step ? total - offset : step));
            memcpy(&s_master_frame[offset], part, part_length);
        }
        frame = s_master_frame;
        length = total;
    }
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

#if defined(NRF_TWIS_RUNTIME_INTEGRATION)
static void test_address_only_status_select_with_stale_dma_count(void)
{
    nrfx_twis_config_t config = {0};
    assert(__wrap_nrfx_twis_init(&s_test_twis, &config, test_twis_shim) == 0);
    nrf_boot_twis_record_t drained;
    while (nrf_bootstrap_runtime_zephyr_drain_twis_record(&drained)) {}
    const nrf_upstream_status_t before = read_status(NULL);
    send_log_status(0xAA55U);
    uint8_t *response;
    uint32_t length;
    assert(s_callbacks->buf_read_requested(NULL, &response, &length) == 0);
    assert(response[3] == NRF_LOG_MESSAGE_STATUS);

    /* ESP32's address-only selector follows a previous 12-byte write. nrfx
     * reports the old AMOUNT; the guard must reject the payload but end LOG selection. */
    test_upstream_registers.rx_amount = 12U;
    memset(s_test_rx, 0xFF, sizeof(s_test_rx));
    nrfx_twis_event_t event = {.type = NRFX_TWIS_EVT_WRITE_REQ, .data.buf_req = true};
    s_test_twis_handler(&event);
    s_cycle += 10U;
    event.type = NRFX_TWIS_EVT_WRITE_DONE;
    s_test_twis_handler(&event);
    bool rejected_stale_payload = false;
    while (nrf_bootstrap_runtime_zephyr_drain_twis_record(&drained)) {
        assert(drained.direction != NRF_BOOT_TWIS_DIRECTION_WRITE_ERROR);
        if (drained.direction == NRF_BOOT_TWIS_DIRECTION_WRITE_STALE_AMOUNT) {
            assert(drained.result == -EPROTO && drained.rx_amount == 12U);
            assert(drained.event_dt_us == 11U && drained.prefix_length == 0U);
            rejected_stale_payload = true;
        }
    }
    assert(rejected_stale_payload);
    const nrf_upstream_status_t after = read_status(NULL);
    assert(after.request_id == before.request_id && after.result == before.result);

    /* A valid log request must still work after the rejected completion. */
    send_log_status(0xAA56U);
    assert(s_callbacks->buf_read_requested(NULL, &response, &length) == 0);
    assert(response[3] == NRF_LOG_MESSAGE_STATUS);
    s_callbacks->buf_write_received(NULL, NULL, 0U);
}

#endif
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
    frame = read_frame(&length);
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
    static uint32_t request_id = 0xD0000000U;
    nrf_log_status_t status;
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH];
    size_t length = 0U;
    assert(nrf_log_runtime_zephyr_build_status(
               request_id++,
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

static nrf_cs_control_request_t make_cs_request(uint32_t request_id, uint32_t attempt_id)
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

static void send_cs_execute(const nrf_cs_control_request_t *request)
{
    uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH];

    assert(nrf_cs_control_encode_execute(request, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
}

static void test_malformed_control_lengths_preserve_assignment(void)
{
    static const struct {
        uint8_t type;
        uint8_t length;
    } commands[] = {
        {NRF_CS_CONTROL_MESSAGE_EXECUTE, NRF_CS_CONTROL_EXECUTE_LENGTH},
        {NRF_CS_CONTROL_MESSAGE_STATUS_REQUEST, NRF_CS_CONTROL_STATUS_REQUEST_LENGTH},
        {NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH, NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH},
        {NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE, NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH},
        {NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE, NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH},
        {NRF_LOG_MESSAGE_FIRMWARE_REQUEST, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH},
    };
    const nrf_upstream_status_t before = read_status(NULL);
    const unsigned submits_before = s_submit_calls;
    const unsigned refreshes_before = s_sensor_refresh_calls;
    const unsigned heights_before = s_height_reference_calls;
    const unsigned calibrations_before = s_imu_calibration_calls;

    assert(before.result == NRF_UPSTREAM_RESULT_OK);
    assert(before.role == NRF_UPSTREAM_ROLE_REMOTE);
    for (size_t index = 0U; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        uint8_t frame[NRF_CS_CONTROL_EXECUTE_LENGTH + 1U] = {'G', 'S', 1U};
        const uint32_t lengths[] = {
            commands[index].length - 1U,
            commands[index].length + 1U,
            4U,
        };
        frame[3] = commands[index].type;
        frame[4] = commands[index].length;
        frame[6] = 0xA5U;
        for (size_t length = 0U; length < sizeof(lengths) / sizeof(lengths[0]); ++length) {
            s_callbacks->buf_write_received(NULL, frame, lengths[length]);
            if (commands[index].type == NRF_LOG_MESSAGE_FIRMWARE_REQUEST) {
                const nrf_upstream_status_t rejected = read_status(NULL);
                assert(rejected.result == NRF_UPSTREAM_RESULT_INVALID_FRAME);
                s_callbacks->buf_write_received(NULL, NULL, 0U);
            }
            const nrf_upstream_status_t after = read_status(NULL);
            assert(memcmp(&before, &after, sizeof(before)) == 0);
        }
    }
    assert(s_submit_calls == submits_before);
    assert(s_sensor_refresh_calls == refreshes_before);
    assert(s_height_reference_calls == heights_before);
    assert(s_imu_calibration_calls == calibrations_before);
}

static void test_firmware_query_preserves_assignment(void)
{
    const nrf_upstream_status_t before = read_status(NULL);
    nrf_log_status_request_t request = {.request_id = 0xF1234567U};
    uint8_t query[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint32_t length = 0U;
    nrf_log_firmware_info_t info;
    assert(nrf_log_protocol_encode_firmware_request(&request, query) == NRF_LOG_ERROR_OK);
    s_callbacks->buf_write_received(NULL, query, sizeof(query));
    uint8_t *frame = read_frame(&length);
    assert(nrf_log_protocol_decode_firmware(frame, length, &info) == NRF_LOG_ERROR_OK);
    assert(info.request_id == request.request_id);
    assert(info.boot_id == nrf_log_runtime_zephyr_boot_id());
    assert(info.git_commit[0] == 0x00U && info.git_commit[19] == 0x67U);
    s_callbacks->buf_write_received(NULL, NULL, 0U);
    nrf_upstream_status_t after = read_status(NULL);
    assert(memcmp(&before, &after, sizeof(before)) == 0);

    query[sizeof(query) - 1U] ^= 1U;
    s_callbacks->buf_write_received(NULL, query, sizeof(query));
    s_callbacks->buf_write_received(NULL, NULL, 0U);
    after = read_status(NULL);
    assert(memcmp(&before, &after, sizeof(before)) == 0);
}

static nrf_cs_control_result_t read_cs_result_for_request(
    const nrf_cs_control_request_t *request)
{
    nrf_cs_control_result_t result;
    uint8_t frame[NRF_CS_CONTROL_STATUS_REQUEST_LENGTH];
    uint8_t *response;
    uint32_t length;

    assert(nrf_cs_control_encode_status_request(request, frame) == 0);
    s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
    response = read_frame(&length);
    assert(length == NRF_CS_CONTROL_RESULT_LENGTH);
    assert(nrf_cs_control_decode_result(response, length, &result) == 0);
    return result;
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

static void run_delayed_work(void)
{
    struct k_work_delayable *work = s_delayed_work;
    assert(work != NULL && work->work.handler != NULL);
    s_delayed_work = NULL;
    work->work.handler(&work->work);
}

static void run_submitted_work(void)
{
    struct k_work *work = s_submitted_work;
    assert(work != NULL && work->handler != NULL);
    s_submitted_work = NULL;
    work->queued = false;
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

/*
 * Contract of the TWIS log path between the ESP32 master and this target.
 *
 * The nrfx TWIS shim (ncs v3.4.0 zephyr/drivers/i2c/i2c_nrfx_twis.c) only prepares
 * the transmit DMA when buf_read_requested returns 0 and the buffer fits its DMA
 * buffer (CONFIG_I2C_NRFX_TWIS_BUF_SIZE=144 in nRF54L15/zephyr/prj.conf). In every
 * other case it logs and returns, the peripheral keeps SCL low and the ESP32
 * aborts with "I2C transaction timeout detected" (the symptom seen on MAIN).
 * RX is cut at the same DMA buffer size. This model applies exactly those rules,
 * so no request, however malformed, may leave the target without a response.
 */
#define TWIS_DMA_BUFFER_SIZE 144U

static unsigned s_twis_contract_cases;

typedef struct {
    bool stalled;
    uint32_t length; /* what the TWIS transmits for this read: at most one window */
    uint32_t total;  /* the whole response the master is reading; the rest follows as windows */
    uint8_t data[TWIS_DMA_BUFFER_SIZE];
} twis_response_t;

static twis_response_t twis_read(void)
{
    twis_response_t response;
    uint8_t *buffer = NULL;
    uint32_t size = 0U;

    memset(&response, 0, sizeof(response));
    const int result = s_callbacks->buf_read_requested(NULL, &buffer, &size);
    response.stalled = result != 0 || buffer == NULL || size == 0U || size > TWIS_DMA_BUFFER_SIZE;
    /* No read may leave transmit data pending past the one window the master clocks out. */
    assert(response.stalled || size <= NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES);
    if (!response.stalled) {
        response.length = size;
        memcpy(response.data, buffer, size);
        response.total = nrf_bootstrap_runtime_zephyr_response_length();
        if (response.total < size) response.total = size;
    }
    return response;
}

static twis_response_t twis_window_read(uint16_t offset);

/* The master's whole response: the first window already read plus the rest through READ_WINDOW requests. */
static twis_response_t twis_complete(const twis_response_t *first)
{
    const uint32_t step = NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
    twis_response_t whole = *first;

    assert(!first->stalled && first->total <= TWIS_DMA_BUFFER_SIZE);
    for (uint32_t offset = step; offset < first->total; offset += step) {
        const uint32_t want = first->total - offset < step ? first->total - offset : step;
        const twis_response_t window = twis_window_read((uint16_t)offset);

        assert(!window.stalled && window.length == want);
        memcpy(&whole.data[offset], window.data, want);
    }
    whole.length = first->total;
    return whole;
}

static twis_response_t twis_write_then_read(const uint8_t *frame, uint32_t length)
{
    uint8_t rx[TWIS_DMA_BUFFER_SIZE] = {0};
    const uint32_t accepted = length > TWIS_DMA_BUFFER_SIZE ? TWIS_DMA_BUFFER_SIZE : length;

    if (accepted != 0U) {
        memcpy(rx, frame, accepted);
    }
    s_callbacks->buf_write_received(NULL, rx, accepted);
    return twis_read();
}

typedef struct {
    unsigned submits;
    unsigned refreshes;
    unsigned heights;
    unsigned calibrations;
    unsigned reboots;
    unsigned readdresses;
    unsigned assignments;
    nrf_upstream_status_t status;
} twis_state_t;

static twis_state_t twis_state(void)
{
    twis_state_t state = {
        .submits = s_submit_calls,
        .refreshes = s_sensor_refresh_calls,
        .heights = s_height_reference_calls,
        .calibrations = s_imu_calibration_calls,
        .reboots = s_reboot_calls,
        .readdresses = s_readdress_calls,
        .assignments = s_sensor_assignment_calls,
    };
    /* A log reply stays selected until the next write; a zero-length write ends it. */
    s_callbacks->buf_write_received(NULL, NULL, 0U);
    state.status = read_status(NULL);
    return state;
}

/* A rejected request must not start work or change the assignment. */
static void twis_assert_state_unchanged(const twis_state_t *before)
{
    const twis_state_t after = twis_state();

    assert(after.submits == before->submits);
    assert(after.refreshes == before->refreshes);
    assert(after.heights == before->heights);
    assert(after.calibrations == before->calibrations);
    assert(after.reboots == before->reboots);
    assert(after.readdresses == before->readdresses);
    assert(after.assignments == before->assignments);
    assert(after.status.role == before->status.role);
    assert(after.status.node_id == before->status.node_id);
    assert(after.status.mux_channel == before->status.mux_channel);
    assert(after.status.target_address == before->status.target_address);
    assert(after.status.lifecycle == before->status.lifecycle);
    assert(after.status.flags == before->status.flags);
    assert(after.status.hardware_id_length == before->status.hardware_id_length);
    assert(memcmp(after.status.hardware_id, before->status.hardware_id,
                  sizeof(after.status.hardware_id)) == 0);
}

static bool twis_is_log_reply(const twis_response_t *response)
{
    return response->length > 3U &&
           (response->data[3] == NRF_LOG_MESSAGE_STATUS ||
            response->data[3] == NRF_LOG_MESSAGE_BLOCK ||
            response->data[3] == NRF_LOG_MESSAGE_FIRMWARE);
}

/* The reply to a rejected write is the ordinary upstream STATUS (or snapshot). */
static void twis_assert_upstream_status(const twis_response_t *response)
{
    nrf_upstream_status_t status;

    assert(!response->stalled);
    assert(response->length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH ||
           response->length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    assert(!twis_is_log_reply(response));
    assert(nrf_upstream_protocol_decode_status(
               response->data, NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH, &status) ==
           NRF_UPSTREAM_RESULT_OK);
}

static void twis_refresh_crc(uint8_t *frame, size_t length)
{
    const uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, length - 2U);

    frame[length - 2U] = (uint8_t)crc;
    frame[length - 1U] = (uint8_t)(crc >> 8U);
}

static void twis_check_log_status(uint32_t request_id)
{
    const nrf_log_status_request_t request = {.request_id = request_id};
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    nrf_log_status_t status;

    assert(nrf_log_protocol_encode_status_request(&request, frame) == NRF_LOG_ERROR_OK);
    const twis_response_t window0 = twis_write_then_read(frame, sizeof(frame));
    ++s_twis_contract_cases;
    assert(!window0.stalled && window0.total == NRF_LOG_PROTOCOL_STATUS_LENGTH);
    assert(window0.length == NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES); /* one window per read */
    /* A retried first window gets the same bytes. */
    for (unsigned repeat = 0U; repeat < 3U; ++repeat) {
        const twis_response_t again = twis_read();
        assert(!again.stalled && again.length == window0.length && again.total == window0.total);
        assert(memcmp(again.data, window0.data, window0.length) == 0);
    }
    const twis_response_t first = twis_complete(&window0);
    assert(first.data[3] == NRF_LOG_MESSAGE_STATUS);
    assert(nrf_log_protocol_decode_status(first.data, first.length, &status) == NRF_LOG_ERROR_OK);
    assert(status.request_id == request_id);
    assert(status.boot_id == nrf_log_runtime_zephyr_boot_id());
    assert(status.next_seq >= status.oldest_seq);
    assert(status.available == status.next_seq - status.oldest_seq);
}

static nrf_log_block_t twis_check_log_block(
    uint32_t request_id,
    uint32_t expected_boot_id,
    uint32_t cursor_seq,
    uint8_t max_records)
{
    const nrf_log_read_request_t request = {
        .request_id = request_id,
        .expected_boot_id = expected_boot_id,
        .cursor_seq = cursor_seq,
        .max_records = max_records,
    };
    uint8_t frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    nrf_log_block_t block;
    nrf_log_block_validation_t validation = {
        .has_expected_boot_id = true,
        .expected_boot_id = expected_boot_id,
        .has_max_records = true,
        .max_records = max_records,
    };

    assert(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_OK);
    const twis_response_t window0 = twis_write_then_read(frame, sizeof(frame));
    ++s_twis_contract_cases;
    assert(!window0.stalled);
    assert(window0.length <= NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES);
    const twis_response_t again = twis_read(); /* a retried first window */
    assert(!again.stalled && again.length == window0.length && again.total == window0.total);
    assert(memcmp(again.data, window0.data, window0.length) == 0);
    const twis_response_t first = twis_complete(&window0);
    assert(first.data[3] == NRF_LOG_MESSAGE_BLOCK);
    assert(nrf_log_protocol_decode_block(first.data, first.length, &validation, &block) ==
           NRF_LOG_ERROR_OK);
    assert(block.request_id == request_id);
    assert(block.record_count <= max_records);
    /* The ESP32 reads exactly base + records * record_length bytes for max_records. */
    assert(first.length == NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
                           (uint32_t)NRF_LOG_PROTOCOL_RECORD_LENGTH * block.record_count);
    assert(first.length <= NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
                           (uint32_t)NRF_LOG_PROTOCOL_RECORD_LENGTH * max_records);
    assert(first.length <= TWIS_DMA_BUFFER_SIZE);
    return block;
}

static void twis_check_log_firmware(uint32_t request_id)
{
    const nrf_log_status_request_t request = {.request_id = request_id};
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    nrf_log_firmware_info_t info;

    assert(nrf_log_protocol_encode_firmware_request(&request, frame) == NRF_LOG_ERROR_OK);
    const twis_response_t window0 = twis_write_then_read(frame, sizeof(frame));
    ++s_twis_contract_cases;
    assert(!window0.stalled && window0.total == NRF_LOG_PROTOCOL_FIRMWARE_LENGTH);
    const twis_response_t response = twis_complete(&window0);
    assert(response.data[3] == NRF_LOG_MESSAGE_FIRMWARE);
    assert(nrf_log_protocol_decode_firmware(response.data, response.length, &info) ==
           NRF_LOG_ERROR_OK);
    assert(info.request_id == request_id);
    assert(info.boot_id == nrf_log_runtime_zephyr_boot_id());
}

static void twis_check_rejected(const uint8_t *frame, uint32_t length, const twis_state_t *before)
{
    const twis_response_t response = twis_write_then_read(frame, length);

    ++s_twis_contract_cases;
    twis_assert_upstream_status(&response);
    twis_assert_state_unchanged(before);
}

static void test_twis_log_contract_never_leaves_the_target_without_a_response(void)
{
    uint8_t status_request[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint8_t firmware_request[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint8_t read_request[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    const struct {
        const uint8_t *frame;
        uint32_t length;
    } valid[] = {
        {status_request, sizeof(status_request)},
        {firmware_request, sizeof(firmware_request)},
        {read_request, sizeof(read_request)},
    };
    nrf_log_status_t log_status;

    /* Enough history for a full block, a partial block and an empty one. */
    for (uint32_t i = 0U; i < 12U; ++i) {
        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_BOOT, i, 0U);
    }
    log_status = read_log_status();
    assert(log_status.next_seq - log_status.oldest_seq >= 12U);

    /* Valid requests: every record count, every cursor class, repeated reads. */
    twis_check_log_status(0x5100U);
    twis_check_log_firmware(0x5200U);
    for (uint8_t max_records = 1U; max_records <= NRF_LOG_PROTOCOL_MAX_RECORDS; ++max_records) {
        const uint32_t cursors[] = {
            log_status.oldest_seq,
            log_status.oldest_seq + 3U,
            log_status.next_seq - 1U,
            log_status.next_seq,
        };
        for (size_t c = 0U; c < sizeof(cursors) / sizeof(cursors[0]); ++c) {
            const uint32_t pending = log_status.next_seq - cursors[c];
            const nrf_log_block_t block = twis_check_log_block(
                0x5300U + max_records * 16U + (uint32_t)c, log_status.boot_id, cursors[c], max_records);
            assert(block.record_count == (pending < max_records ? pending : max_records));
            assert(((block.flags & NRF_LOG_BLOCK_FLAG_MORE) != 0U) == (pending > block.record_count));
        }
        /* A reboot between STATUS and READ: empty block flagged BOOT_MISMATCH, 36 bytes. */
        const nrf_log_block_t mismatch = twis_check_log_block(
            0x5400U + max_records, log_status.boot_id ^ 0x5A5A5A5AU, log_status.oldest_seq, max_records);
        assert(mismatch.record_count == 0U);
        assert((mismatch.flags & NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH) != 0U);
    }

    {
        const nrf_log_status_request_t status_id = {.request_id = 0x5501U};
        const nrf_log_status_request_t firmware_id = {.request_id = 0x5502U};
        const nrf_log_read_request_t read_id = {
            .request_id = 0x5503U,
            .expected_boot_id = log_status.boot_id,
            .cursor_seq = log_status.oldest_seq,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        assert(nrf_log_protocol_encode_status_request(&status_id, status_request) == NRF_LOG_ERROR_OK);
        assert(nrf_log_protocol_encode_firmware_request(&firmware_id, firmware_request) == NRF_LOG_ERROR_OK);
        assert(nrf_log_protocol_encode_read_request(&read_id, read_request) == NRF_LOG_ERROR_OK);
    }

    const twis_state_t before = twis_state();

    /* Any single flipped bit: the CRC rejects it, the target still answers. */
    for (size_t v = 0U; v < sizeof(valid) / sizeof(valid[0]); ++v) {
        for (uint32_t byte = 0U; byte < valid[v].length; ++byte) {
            for (unsigned bit = 0U; bit < 8U; ++bit) {
                uint8_t corrupted[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
                memcpy(corrupted, valid[v].frame, valid[v].length);
                corrupted[byte] ^= (uint8_t)(1U << bit);
                twis_check_rejected(corrupted, valid[v].length, &before);
            }
        }
    }

    /* Truncated, padded and over-long writes (the DMA buffer cuts RX at 144 bytes). */
    for (size_t v = 0U; v < sizeof(valid) / sizeof(valid[0]); ++v) {
        static const uint32_t extra[] = {1U, 2U, 3U, 4U, 20U, 100U, 120U, 132U, 150U};
        for (uint32_t length = 0U; length < valid[v].length; ++length) {
            twis_check_rejected(valid[v].frame, length, &before);
        }
        for (size_t e = 0U; e < sizeof(extra) / sizeof(extra[0]); ++e) {
            uint8_t padded[TWIS_DMA_BUFFER_SIZE + 16U] = {0};
            memcpy(padded, valid[v].frame, valid[v].length);
            twis_check_rejected(padded, valid[v].length + extra[e], &before);
            memset(padded + valid[v].length, 0xFF, sizeof(padded) - valid[v].length);
            twis_check_rejected(padded, valid[v].length + extra[e], &before);
        }
    }
    twis_check_rejected(NULL, 0U, &before);

    /* Well-formed frames (valid CRC) carrying out-of-range fields. */
    {
        uint8_t forged[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
        static const uint8_t bad_counts[] = {0U, 6U, 7U, 16U, 128U, 255U};

        for (size_t i = 0U; i < sizeof(bad_counts); ++i) {
            memcpy(forged, read_request, sizeof(forged));
            forged[18] = bad_counts[i];
            twis_refresh_crc(forged, sizeof(forged));
            twis_check_rejected(forged, sizeof(forged), &before);
        }
        for (unsigned value = 1U; value < 256U; value += 17U) {
            memcpy(forged, read_request, sizeof(forged));
            forged[19] = (uint8_t)value; /* reserved byte */
            twis_refresh_crc(forged, sizeof(forged));
            twis_check_rejected(forged, sizeof(forged), &before);
        }
        for (unsigned version = 0U; version < 256U; version += 51U) {
            if (version == NRF_LOG_PROTOCOL_VERSION) continue;
            memcpy(forged, read_request, sizeof(forged));
            forged[2] = (uint8_t)version;
            twis_refresh_crc(forged, sizeof(forged));
            twis_check_rejected(forged, sizeof(forged), &before);
        }
        for (unsigned declared = 0U; declared < 64U; declared += 7U) {
            if (declared == sizeof(forged)) continue;
            memcpy(forged, read_request, sizeof(forged));
            forged[4] = (uint8_t)declared; /* declared length differs from the wire length */
            twis_refresh_crc(forged, sizeof(forged));
            twis_check_rejected(forged, sizeof(forged), &before);
        }
        /* A cursor beyond next_seq for the current boot is an invariant violation, answered
         * with the error STATUS, never with silence. */
        memcpy(forged, read_request, sizeof(forged));
        forged[14] = 0xFFU;
        forged[15] = 0xFFU;
        forged[16] = 0xFFU;
        forged[17] = 0x7FU;
        twis_refresh_crc(forged, sizeof(forged));
        twis_check_rejected(forged, sizeof(forged), &before);
    }

    /* A valid request after all that noise is answered normally. */
    twis_check_log_status(0x5600U);
    twis_check_log_block(0x5601U, log_status.boot_id, log_status.oldest_seq, 2U);
    twis_check_log_firmware(0x5602U);
    /* 368 single-bit flips, 46 truncations, 54 padded writes, 1 empty write, 38 forged frames, 30 valid exchanges. */
    assert(s_twis_contract_cases == 537U);
}

/*
 * Windowed reads. The ESP32 reads responses longer than 28 bytes as windows
 * because every segmented read of the 2026-10-26 wire capture arrived damaged.
 * Same shim model as above: the buffer returned by buf_read_requested is what
 * the TWIS would transmit, and the master clocks out only as many bytes as it asks.
 */
static twis_response_t twis_window_read(uint16_t offset)
{
    const nrf_upstream_read_window_t window = {.offset = offset};
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];

    assert(nrf_upstream_protocol_encode_read_window(&window, frame) == NRF_UPSTREAM_RESULT_OK);
    return twis_write_then_read(frame, sizeof(frame));
}

/* What the ESP32 does after the first read: one window request + read per 28 bytes. */
static void twis_assemble_windows(const twis_response_t *first, uint32_t total, uint8_t *out)
{
    const uint32_t step = NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
    uint32_t offset;

    assert(!first->stalled && first->total == total);
    assert(first->length == (total < step ? total : step)); /* never more than one window per read */
    memcpy(out, first->data, first->length);
    for (offset = step; offset < total; offset += step) {
        const uint32_t want = total - offset < step ? total - offset : step;
        const twis_response_t window = twis_window_read((uint16_t)offset);

        assert(!window.stalled);
        assert(window.length == want); /* exactly the bytes the master clocks out, nothing left over */
        memcpy(&out[offset], window.data, want);
    }
}

static void twis_send_raw(const uint8_t *frame, uint32_t length)
{
    s_callbacks->buf_write_received(NULL, (uint8_t *)frame, length);
}

static void twis_select_log_status_reply(uint32_t request_id)
{
    const nrf_log_status_request_t request = {.request_id = request_id};
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    twis_response_t reply;

    assert(nrf_log_protocol_encode_status_request(&request, frame) == NRF_LOG_ERROR_OK);
    reply = twis_write_then_read(frame, sizeof(frame));
    assert(!reply.stalled && reply.total == NRF_LOG_PROTOCOL_STATUS_LENGTH &&
           reply.data[3] == NRF_LOG_MESSAGE_STATUS);
}

/* A write that is not a valid window request must leave an ordinary read behind. */
static void twis_expect_window_write_refused(const uint8_t *frame, uint32_t length)
{
    twis_response_t reply;

    twis_select_log_status_reply(0x7200U);
    twis_send_raw(frame, length);
    reply = twis_read();
    twis_assert_upstream_status(&reply);
}

static void test_twis_read_windows_continue_the_selected_response(void)
{
    nrf_log_status_t log_status;
    uint8_t assembled[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];

    for (uint32_t i = 0U; i < 12U; ++i) {
        nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_BOOT, 100U + i, 0U);
    }
    log_status = read_log_status();
    assert(log_status.next_seq - log_status.oldest_seq >= 12U);

    /* 38 byte log status: the ordinary read, then one window completes it. */
    {
        const nrf_log_status_request_t request = {.request_id = 0x7100U};
        uint8_t request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
        nrf_log_status_t decoded;
        twis_response_t next;

        assert(nrf_log_protocol_encode_status_request(&request, request_frame) == NRF_LOG_ERROR_OK);
        const twis_response_t first = twis_write_then_read(request_frame, sizeof(request_frame));
        assert(!first.stalled && first.total == NRF_LOG_PROTOCOL_STATUS_LENGTH);
        twis_assemble_windows(&first, NRF_LOG_PROTOCOL_STATUS_LENGTH, assembled);
        assert(memcmp(assembled, first.data, first.length) == 0);
        assert(nrf_log_protocol_decode_status(
                   assembled, NRF_LOG_PROTOCOL_STATUS_LENGTH, &decoded) == NRF_LOG_ERROR_OK);
        assert(decoded.request_id == 0x7100U);
        /* The window request ended the log reply; the next ordinary read is the STATUS. */
        next = twis_read();
        twis_assert_upstream_status(&next);
    }

    /* Full block of five records (136 bytes): four windows after the ordinary read. */
    {
        const nrf_log_read_request_t request = {
            .request_id = 0x7101U,
            .expected_boot_id = log_status.boot_id,
            .cursor_seq = log_status.oldest_seq,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        const nrf_log_block_validation_t validation = {
            .has_expected_boot_id = true,
            .expected_boot_id = log_status.boot_id,
            .has_max_records = true,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        uint8_t request_frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
        uint8_t second[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
        nrf_log_block_t block;
        twis_response_t next;

        assert(nrf_log_protocol_encode_read_request(&request, request_frame) == NRF_LOG_ERROR_OK);
        const twis_response_t first = twis_write_then_read(request_frame, sizeof(request_frame));
        assert(!first.stalled && first.total == NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH);
        twis_assemble_windows(&first, first.total, assembled);
        assert(memcmp(assembled, first.data, first.length) == 0);
        assert(nrf_log_protocol_decode_block(assembled, first.total, &validation, &block) == NRF_LOG_ERROR_OK);
        assert(block.request_id == 0x7101U && block.record_count == NRF_LOG_PROTOCOL_MAX_RECORDS);

        /* The windows are the frozen block even after the log has moved on. */
        assert(nrf_log_protocol_encode_read_request(&request, request_frame) == NRF_LOG_ERROR_OK);
        {
            const twis_response_t again = twis_write_then_read(request_frame, sizeof(request_frame));
            nrf_log_runtime_zephyr_append_event(NRF_LOG_EVENT_BOOT, 0xBEEFU, 0U);
            assert(again.total == first.total && again.length == first.length &&
                   memcmp(again.data, first.data, first.length) == 0);
            twis_assemble_windows(&again, again.total, second);
            assert(memcmp(second, assembled, first.total) == 0);
        }
        next = twis_read();
        twis_assert_upstream_status(&next);
    }

    /* A block with fewer records is windowed over its own, shorter length. */
    {
        const nrf_log_read_request_t request = {
            .request_id = 0x7102U,
            .expected_boot_id = log_status.boot_id,
            .cursor_seq = log_status.oldest_seq,
            .max_records = 2U,
        };
        uint8_t request_frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
        nrf_log_block_t block;

        assert(nrf_log_protocol_encode_read_request(&request, request_frame) == NRF_LOG_ERROR_OK);
        const twis_response_t first = twis_write_then_read(request_frame, sizeof(request_frame));
        assert(!first.stalled && first.total == NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH + 2U * NRF_LOG_PROTOCOL_RECORD_LENGTH);
        twis_assemble_windows(&first, first.total, assembled);
        assert(nrf_log_protocol_decode_block(assembled, first.total, NULL, &block) == NRF_LOG_ERROR_OK);
        assert(block.record_count == 2U);
    }

    /* Firmware reply (37 bytes). */
    {
        const nrf_log_status_request_t request = {.request_id = 0x7103U};
        uint8_t request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
        nrf_log_firmware_info_t info;

        assert(nrf_log_protocol_encode_firmware_request(&request, request_frame) == NRF_LOG_ERROR_OK);
        const twis_response_t first = twis_write_then_read(request_frame, sizeof(request_frame));
        assert(!first.stalled && first.total == NRF_LOG_PROTOCOL_FIRMWARE_LENGTH);
        twis_assemble_windows(&first, NRF_LOG_PROTOCOL_FIRMWARE_LENGTH, assembled);
        assert(nrf_log_protocol_decode_firmware(
                   assembled, NRF_LOG_PROTOCOL_FIRMWARE_LENGTH, &info) == NRF_LOG_ERROR_OK);
        assert(info.request_id == 0x7103U);
    }

    /* The ordinary upstream reply (STATUS, or the 136 byte snapshot on a REMOTE). */
    {
        uint8_t bytes[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
        twis_response_t first;

        s_callbacks->buf_write_received(NULL, NULL, 0U); /* ends any selected reply */
        first = twis_read();
        twis_assert_upstream_status(&first);
        if (first.total > NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES) {
            twis_assemble_windows(&first, first.total, bytes);
            assert(memcmp(bytes, first.data, first.length) == 0);
        } else {
            /* A 28 byte reply has nothing past its first window: asking for one is refused. */
            const twis_response_t refused = twis_window_read(NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES);
            twis_assert_upstream_status(&refused);
        }
    }
}

static void test_twis_read_window_requests_are_validated_and_one_shot(void)
{
    const nrf_upstream_read_window_t window = {.offset = NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES};
    const uint32_t window_slice = NRF_LOG_PROTOCOL_STATUS_LENGTH - NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES;
    uint8_t good[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
    const twis_state_t before = twis_state();

    assert(nrf_upstream_protocol_encode_read_window(&window, good) == NRF_UPSTREAM_RESULT_OK);

    /* A valid request is honoured exactly once. */
    {
        twis_response_t first;
        twis_response_t served;
        twis_response_t next;

        twis_select_log_status_reply(0x7200U);
        first = twis_read();
        const twis_response_t whole_reference = twis_complete(&first); /* what the window slices must equal */
        twis_select_log_status_reply(0x7200U);
        first = twis_read();
        assert(first.total == NRF_LOG_PROTOCOL_STATUS_LENGTH);
        twis_send_raw(good, sizeof(good));
        served = twis_read();
        assert(!served.stalled && served.length == window_slice);
        assert(memcmp(served.data, &whole_reference.data[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES], window_slice) == 0);
        next = twis_read();
        twis_assert_upstream_status(&next);
    }

    /* Every single-bit corruption of the request is refused. */
    for (uint32_t byte = 0U; byte < sizeof(good); ++byte) {
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint8_t corrupted[sizeof(good)];

            memcpy(corrupted, good, sizeof(good));
            corrupted[byte] ^= (uint8_t)(1U << bit);
            twis_expect_window_write_refused(corrupted, sizeof(corrupted));
        }
    }

    /* Truncated, padded and over-long writes are refused. */
    for (uint32_t length = 0U; length < sizeof(good); ++length) {
        twis_expect_window_write_refused(good, length);
    }
    {
        uint8_t padded[TWIS_DMA_BUFFER_SIZE] = {0};
        static const uint32_t lengths[] = {13U, 14U, 16U, 24U, 100U, 144U};

        memcpy(padded, good, sizeof(good));
        for (size_t i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            twis_expect_window_write_refused(padded, lengths[i]);
        }
    }

    /* Well-formed frames (valid CRC) with fields out of range. */
    {
        uint8_t forged[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH];
        /* the selected reply is 38 bytes: 38 and above is past its end */
        static const uint16_t past_the_reply[] = {38U, 39U, 100U, 135U};
        static const uint16_t outside_any_response[] = {136U, 137U, 255U, 256U, 0xFFFFU};

        for (size_t i = 0U; i < sizeof(past_the_reply) / sizeof(past_the_reply[0]); ++i) {
            memcpy(forged, good, sizeof(forged));
            forged[6] = (uint8_t)past_the_reply[i];
            forged[7] = (uint8_t)(past_the_reply[i] >> 8U);
            twis_refresh_crc(forged, sizeof(forged));
            twis_expect_window_write_refused(forged, sizeof(forged));
            assert(find_log_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME).arg0 == NRF_UPSTREAM_MESSAGE_READ_WINDOW);
            assert(find_log_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME).arg1 == past_the_reply[i]);
        }
        for (size_t i = 0U; i < sizeof(outside_any_response) / sizeof(outside_any_response[0]); ++i) {
            memcpy(forged, good, sizeof(forged));
            forged[6] = (uint8_t)outside_any_response[i];
            forged[7] = (uint8_t)(outside_any_response[i] >> 8U);
            twis_refresh_crc(forged, sizeof(forged));
            twis_expect_window_write_refused(forged, sizeof(forged));
            assert(find_log_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME).arg1 == NRF_UPSTREAM_RESULT_INVALID_FRAME);
        }
        for (unsigned reserved = 8U; reserved <= 9U; ++reserved) {
            memcpy(forged, good, sizeof(forged));
            forged[reserved] = 0x5AU;
            twis_refresh_crc(forged, sizeof(forged));
            twis_expect_window_write_refused(forged, sizeof(forged));
        }
    }

    /* Any other write between the window request and the read cancels it. */
    {
        twis_response_t reply;

        twis_select_log_status_reply(0x7201U);
        twis_send_raw(good, sizeof(good));
        s_callbacks->buf_write_received(NULL, NULL, 0U); /* address-only probe */
        reply = twis_read();
        twis_assert_upstream_status(&reply);

        twis_select_log_status_reply(0x7202U);
        twis_send_raw(good, sizeof(good));
        twis_select_log_status_reply(0x7203U); /* a new log request instead of the read */
        reply = twis_read();
        assert(!reply.stalled && reply.total == NRF_LOG_PROTOCOL_STATUS_LENGTH);
    }

    twis_assert_state_unchanged(&before);
}

/* characterization: host integration baseline for the real TWIS callbacks and work handlers. */

/* Autonomous Channel Sounding cycle through the real TWIS callbacks: plan, GO, status, results, release. */
static void cycle_send(const uint8_t *frame, size_t length)
{
    s_callbacks->buf_write_received(NULL, (uint8_t *)frame, (uint32_t)length);
}

/* A remote answers the legacy read with its 136 byte snapshot, a main node with the 28 byte STATUS. */
static bool cycle_is_legacy_reply(const uint8_t *frame, uint32_t length)
{
    return (length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH || length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH) &&
        frame[3] == 0x80U;
}

static nrf_cs_cycle_status_t cycle_read_status(void)
{
    uint32_t length;
    uint8_t *frame = read_frame(&length);
    nrf_cs_cycle_status_t status;
    assert(length == NRF_CS_CYCLE_STATUS_LENGTH && frame[3] == NRF_CS_CYCLE_MESSAGE_STATUS);
    assert(nrf_cs_cycle_decode_status(frame, length, &status) == 0);
    return status;
}

extern bool (*nrf_cs_pair_zephyr_stub_final_hint)(const nrf_cs_measurement_t *measurement);
extern void (*nrf_cs_pair_zephyr_stub_released)(void);

/*
 * Relay hooks: the Bluetooth side tells the cycle runtime two things, and both reach it. The plan below is a chain
 * (this node measures the peer, then the peer measures this node), so once its own attempt is done the node only waits
 * to be released as the peer's reflector; the release ends the cycle at once instead of at the end of the last slot.
 */
static void test_relay_hooks_reach_the_cycle_runtime(void)
{
    nrf_upstream_status_t node = read_status(NULL);
    assert(node.role != NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(nrf_cs_pair_zephyr_stub_final_hint != NULL && nrf_cs_pair_zephyr_stub_released != NULL);   /* registered at start */

    const uint8_t self = node.node_id;
    const uint8_t peer = (uint8_t)(self + 1U);
    const uint32_t window = nrf_cs_cycle_window_ms(500U, 1U, 1U);
    uint8_t plan_frame[NRF_CS_CYCLE_PLAN_LENGTH];
    uint8_t steps_frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH];
    uint8_t go_frame[NRF_CS_CYCLE_GO_LENGTH];
    uint8_t request_frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];
    const nrf_cs_cycle_plan_t plan = {
        .request_id = 0x7201U, .cycle_id = 79U, .membership_generation = 1U, .pair_timeout_ms = 500U,
        .max_attempts = 1U, .samples_per_pair = 1U, .min_confidence = 0U, .full_rotation = true,
        .step_count = 2U, .node_count = 2U, .self_node = self,
    };
    nrf_cs_cycle_plan_steps_t steps = {.request_id = 0x7202U, .cycle_id = 79U, .first_step = 0U, .step_count = 2U};
    const nrf_cs_cycle_go_t go = {.request_id = 0x7203U, .cycle_id = 79U, .total_timeout_ms = window * 2U + NRF_CS_CYCLE_TOTAL_MARGIN_MS};
    steps.steps[0] = (nrf_cs_cycle_step_t){.initiator = self, .reflector = peer};
    steps.steps[1] = (nrf_cs_cycle_step_t){.initiator = peer, .reflector = self};

    /* No attempt is running: the hint never says "final", and a release is ignored. */
    const nrf_cs_measurement_t ok = {.outcome = NRF_CS_OUTCOME_SUCCESS, .distance_mm = 1000, .confidence = 900U};
    assert(!nrf_cs_pair_zephyr_stub_final_hint(&ok));
    nrf_cs_pair_zephyr_stub_released();

    assert(nrf_cs_cycle_encode_plan(&plan, plan_frame) == 0);
    cycle_send(plan_frame, sizeof(plan_frame));
    assert(nrf_cs_cycle_encode_plan_steps(&steps, steps_frame) == 0);
    cycle_send(steps_frame, sizeof(steps_frame));
    assert(nrf_cs_cycle_encode_go(&go, go_frame) == 0);
    cycle_send(go_frame, sizeof(go_frame));
    assert(cycle_read_status().state == NRF_CS_CYCLE_STATE_RUNNING);

    /* Its own attempt runs and ends (the stub finishes it at once); the node still has its reflector duty. */
    nrf_cs_pair_runtime_zephyr_stub_configure(0, NRF_CS_OUTCOME_SUCCESS);
    const unsigned executes_before = nrf_cs_pair_zephyr_stub_execute_calls;
    for (unsigned ticks = 0U; ticks < 20000U && nrf_cs_pair_zephyr_stub_execute_calls == executes_before; ++ticks) {
        nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    }
    assert(nrf_cs_pair_zephyr_stub_execute_calls == executes_before + 1U);          /* its own attempt ran */
    for (unsigned ticks = 0U; ticks < 50U; ++ticks) nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    assert(cycle_read_status().state == NRF_CS_CYCLE_STATE_RUNNING);

    /* The peer let go of its link to this node: the cycle is over for this node right away. */
    nrf_cs_pair_zephyr_stub_released();
    nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    const nrf_cs_cycle_status_t done = cycle_read_status();
    assert(done.state == NRF_CS_CYCLE_STATE_COMPLETE && done.pairs_done == 2U);

    const nrf_cs_cycle_result_request_t release = {.request_id = 0x7205U, .cycle_id = 79U, .page = NRF_CS_CYCLE_RESULT_RELEASE_PAGE};
    uint32_t length = 0U;
    assert(nrf_cs_cycle_encode_result_request(&release, request_frame) == 0);
    cycle_send(request_frame, sizeof(request_frame));
    uint8_t *frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length));
}

static void test_autonomous_cs_cycle_messages(nrf_cs_outcome_t outcome)
{
    nrf_upstream_status_t node = read_status(NULL);
    if (node.role == NRF_UPSTREAM_ROLE_UNASSIGNED) {
        const nrf_upstream_assign_t assign = make_assign(0x7000U);
        send_assign(&assign);
        run_submitted_work();
        node = read_status(NULL);
    }
    assert(node.role != NRF_UPSTREAM_ROLE_UNASSIGNED);
    const uint8_t self = node.node_id;
    const uint8_t peer = (uint8_t)(self + 1U);
    const uint32_t window = nrf_cs_cycle_window_ms(500U, 1U, 1U);
    uint8_t plan_frame[NRF_CS_CYCLE_PLAN_LENGTH];
    uint8_t steps_frame[NRF_CS_CYCLE_PLAN_STEPS_LENGTH];
    uint8_t go_frame[NRF_CS_CYCLE_GO_LENGTH];
    uint8_t request_frame[NRF_CS_CYCLE_RESULT_REQUEST_LENGTH];
    nrf_cs_cycle_plan_t plan = {
        .request_id = 0x7101U, .cycle_id = 77U, .membership_generation = 1U, .pair_timeout_ms = 500U,
        .max_attempts = 1U, .samples_per_pair = 1U, .min_confidence = 0U, .full_rotation = true,
        .step_count = 2U, .node_count = 2U, .self_node = self,
    };
    nrf_cs_cycle_plan_steps_t steps = {.request_id = 0x7102U, .cycle_id = 77U, .first_step = 0U, .step_count = 2U};
    const nrf_cs_cycle_go_t go = {.request_id = 0x7103U, .cycle_id = 77U, .total_timeout_ms = window * 2U + NRF_CS_CYCLE_TOTAL_MARGIN_MS};
    nrf_bootstrap_twis_diagnostics_t diag;
    uint32_t length;
    uint8_t *frame;

    steps.steps[0] = (nrf_cs_cycle_step_t){.initiator = self, .reflector = peer};
    steps.steps[1] = (nrf_cs_cycle_step_t){.initiator = peer, .reflector = self};

    /* Idle: STATUS reads stay the legacy 28 byte frame. */
    frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length));
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    const uint32_t errors_before = diag.callback_error_count;

    /* A plan for another node, a corrupted plan and a GO without a plan are refused. */
    plan.self_node = peer;
    assert(nrf_cs_cycle_encode_plan(&plan, plan_frame) == 0);
    cycle_send(plan_frame, sizeof(plan_frame));
    plan.self_node = self;
    assert(nrf_cs_cycle_encode_plan(&plan, plan_frame) == 0);
    plan_frame[12] ^= 0x01U;
    cycle_send(plan_frame, sizeof(plan_frame));
    assert(nrf_cs_cycle_encode_go(&go, go_frame) == 0);
    cycle_send(go_frame, sizeof(go_frame));
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    assert(diag.callback_error_count == errors_before + 3U);
    assert(find_log_event(NRF_LOG_EVENT_TWIS_INVALID_FRAME).arg0 == NRF_CS_CYCLE_MESSAGE_GO);
    frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length)); /* nothing started */

    /* Plan intake: a plan alone is not a cycle, and GO needs every step. */
    assert(nrf_cs_cycle_encode_plan(&plan, plan_frame) == 0);
    cycle_send(plan_frame, sizeof(plan_frame));
    steps.step_count = 1U;
    assert(nrf_cs_cycle_encode_plan_steps(&steps, steps_frame) == 0);
    cycle_send(steps_frame, sizeof(steps_frame));
    cycle_send(go_frame, sizeof(go_frame)); /* refused: step 1 is missing */
    frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length));
    steps.first_step = 1U;
    steps.steps[0] = (nrf_cs_cycle_step_t){.initiator = peer, .reflector = self};
    assert(nrf_cs_cycle_encode_plan_steps(&steps, steps_frame) == 0);
    cycle_send(steps_frame, sizeof(steps_frame));
    frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length)); /* ready but not started: still the legacy reply */

    /* GO: from now on STATUS reads answer with the cycle status. The radio stays silent until the cycle runs. */
    assert(!nrf_cs_pair_zephyr_stub_radio_enabled);
    const unsigned radio_changes_before = nrf_cs_pair_zephyr_stub_radio_changes;
    nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    assert(nrf_cs_pair_zephyr_stub_radio_changes == radio_changes_before);
    cycle_send(go_frame, sizeof(go_frame));
    nrf_cs_cycle_status_t cycle = cycle_read_status();
    assert(cycle.state == NRF_CS_CYCLE_STATE_RUNNING && cycle.cycle_id == 77U && cycle.pairs_total == 2U);
    assert(cycle.request_id == go.request_id && cycle.failure == NRF_CS_CYCLE_FAILURE_NONE);
    cycle_send(go_frame, sizeof(go_frame)); /* a repeated GO changes nothing */
    assert(cycle_read_status().state == NRF_CS_CYCLE_STATE_RUNNING);
    assert(nrf_cs_cycle_encode_plan(&plan, plan_frame) == 0);
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    const uint32_t errors_running = diag.callback_error_count;
    cycle_send(plan_frame, sizeof(plan_frame)); /* no new plan while running */
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    assert(diag.callback_error_count == errors_running + 1U);

    /* The main loop ticks; only window 0 belongs to this node, so exactly one attempt starts. */
    const unsigned executes_before = nrf_cs_pair_zephyr_stub_execute_calls;
    nrf_cs_pair_runtime_zephyr_stub_configure(0, outcome);
    nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    assert(nrf_cs_pair_zephyr_stub_radio_enabled); /* GO opened the radio for the reflector role */
    unsigned ticks = 0U;
    while (ticks < 20000U) {
        nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
        ++ticks;
        if (ticks % 100U == 0U && cycle_read_status().state == NRF_CS_CYCLE_STATE_COMPLETE) break;
    }
    cycle = cycle_read_status();
    assert(cycle.state == NRF_CS_CYCLE_STATE_COMPLETE && cycle.pairs_done == 2U);
    /* Opened once for the whole run and closed again when it ended. */
    nrf_bootstrap_runtime_zephyr_cs_cycle_tick();
    assert(!nrf_cs_pair_zephyr_stub_radio_enabled);
    assert(nrf_cs_pair_zephyr_stub_radio_changes == radio_changes_before + 2U);
    assert(nrf_cs_pair_zephyr_stub_execute_calls == executes_before + 1U);
    const nrf_log_record_t completion = find_log_event(NRF_LOG_EVENT_CS_PAIR_COMPLETE);
    assert(completion.arg0 != 0U);
    assert((completion.arg1 & 0xffU) == (uint32_t)outcome);
    if (outcome == NRF_CS_OUTCOME_TIMEOUT) {
        assert(find_log_event(NRF_LOG_EVENT_CS_TIMEOUT).arg0 == completion.arg0);
    } else if (outcome != NRF_CS_OUTCOME_SUCCESS) {
        assert(find_log_event(NRF_LOG_EVENT_CS_EXECUTE_FAILED).arg0 == completion.arg0);
    }

    /* Results: one page with the single recorded attempt, through the real frame format. */
    const nrf_cs_cycle_result_request_t request = {.request_id = 0x7104U, .cycle_id = 77U, .page = 0U};
    nrf_cs_cycle_result_t result;
    assert(nrf_cs_cycle_encode_result_request(&request, request_frame) == 0);
    cycle_send(request_frame, sizeof(request_frame));
    frame = read_frame(&length);
    assert(length == NRF_CS_CYCLE_RESULT_LENGTH && frame[3] == NRF_CS_CYCLE_MESSAGE_RESULT);
    assert(nrf_cs_cycle_decode_result(frame, length, &result) == 0);
    assert(result.cycle_id == 77U && result.record_total == 1U && result.record_count == 1U);
    assert(result.records[0].execution == 0U && result.records[0].attempt == 1U);
    assert(result.records[0].outcome == outcome);
    /* A request for another cycle or a page that does not exist is refused. */
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    const uint32_t errors_results = diag.callback_error_count;
    nrf_cs_cycle_result_request_t wrong = request;
    wrong.cycle_id = 78U;
    assert(nrf_cs_cycle_encode_result_request(&wrong, request_frame) == 0);
    cycle_send(request_frame, sizeof(request_frame));
    wrong = request;
    wrong.page = 5U;
    assert(nrf_cs_cycle_encode_result_request(&wrong, request_frame) == 0);
    cycle_send(request_frame, sizeof(request_frame));
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diag);
    assert(diag.callback_error_count == errors_results + 2U);
    s_callbacks->buf_write_received(NULL, NULL, 0U); /* an address-only write ends the selected reply */
    assert(cycle_read_status().state == NRF_CS_CYCLE_STATE_COMPLETE);

    /* Release: normal STATUS reads come back. */
    const nrf_cs_cycle_result_request_t release = {.request_id = 0x7105U, .cycle_id = 77U, .page = NRF_CS_CYCLE_RESULT_RELEASE_PAGE};
    assert(nrf_cs_cycle_encode_result_request(&release, request_frame) == 0);
    cycle_send(request_frame, sizeof(request_frame));
    frame = read_frame(&length);
    assert(cycle_is_legacy_reply(frame, length));
}

int main(void)
{
    /* The MAX-494 wrapper keeps using the runtime after this main returns. */
    static nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_bootstrap_twis_diagnostics_t twis_diagnostics;
    nrf_upstream_assign_t assign;
    unsigned i;
    uint32_t read_length;
    uint8_t initial_status_frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH];
    uint8_t *initial_frame;

    memset(&s_sensor_snapshot, 0, sizeof(s_sensor_snapshot));
    s_sensor_snapshot.result = NRF_UPSTREAM_SENSOR_RESULT_NOT_READY;

    assert(nrf_log_runtime_zephyr_init() == 0);
    nrf_node_state_init(&node_state);
    assert(nrf_node_state_mark_ready(&node_state) == NRF_NODE_STATE_OK);
    s_readdress_result = 0;
    s_sensor_assignment_result = 0;
    s_readdress_rollback_discovery = true;
    assert(nrf_bootstrap_runtime_zephyr_start(&node_state) == 0);
    assert(s_work_queue_start_calls == 1U);
    assert(s_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);

    initial_frame = read_frame(&read_length);
    assert(read_length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
    memcpy(initial_status_frame, initial_frame, read_length);
    assert(nrf_upstream_protocol_decode_status(
               initial_frame, read_length, &status) == NRF_UPSTREAM_RESULT_OK);
    assert(status.request_id == 0U && status.role == NRF_UPSTREAM_ROLE_UNASSIGNED);
    assert(status.target_address == NRF_UPSTREAM_ADDRESS_DISCOVERY);
    assert(status.flags == NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED);
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis_diagnostics);
    assert(twis_diagnostics.first_transaction_seen);
    assert(twis_diagnostics.first_transaction_kind ==
           NRF_BOOTSTRAP_TWIS_TRANSACTION_READ_REQUESTED);
    assert(twis_diagnostics.read_requested_count == 1U);
    assert(twis_diagnostics.write_received_count == 0U);

    {
        nrf_log_read_request_t request;
        nrf_log_status_t logs;
        uint8_t *response_frame;
        uint32_t response_length;

        send_log_status(0x7007U);
        s_callbacks->buf_write_received(NULL, NULL, 0U);
        response_frame = read_frame(&response_length);
        assert(response_length == sizeof(initial_status_frame));
        assert(response_frame[3] == 0x80U);
        assert(memcmp(initial_status_frame, response_frame, response_length) == 0);

        logs = read_log_status();
        request = (nrf_log_read_request_t){
            .request_id = 0x8008U,
            .expected_boot_id = logs.boot_id,
            .cursor_seq = logs.next_seq,
            .max_records = NRF_LOG_PROTOCOL_MAX_RECORDS,
        };
        send_log_read(&request);
        s_callbacks->buf_write_received(NULL, NULL, 0U);
        response_frame = read_frame(&response_length);
        assert(response_length == sizeof(initial_status_frame));
        assert(response_frame[3] == 0x80U);
        assert(memcmp(initial_status_frame, response_frame, response_length) == 0);
    }

    {
        const nrf_log_status_t logs_before = read_log_status();
        nrf_bootstrap_twis_diagnostics_t diagnostics_before;
        nrf_bootstrap_twis_diagnostics_t diagnostics_after;
        nrf_boot_twis_record_t record;
        uint8_t *after_frame;
        uint32_t after_length;

        while (nrf_bootstrap_runtime_zephyr_drain_twis_record(&record)) {
        }
        nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diagnostics_before);
        s_callbacks->buf_write_received(NULL, NULL, 0U);
        nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&diagnostics_after);
        assert(nrf_bootstrap_runtime_zephyr_drain_twis_record(&record));
        assert(!nrf_bootstrap_runtime_zephyr_drain_twis_record(&record));
        after_frame = read_frame(&after_length);
        assert(after_length == sizeof(initial_status_frame));
        assert(memcmp(initial_status_frame, after_frame, after_length) == 0);
        assert(nrf_upstream_protocol_decode_status(
                   after_frame, after_length, &status) == NRF_UPSTREAM_RESULT_OK);
        assert(status.request_id == 0U &&
               status.result != NRF_UPSTREAM_RESULT_INVALID_FRAME);
        assert(diagnostics_after.transaction_count ==
               diagnostics_before.transaction_count + 1U);
        assert(diagnostics_after.write_received_count ==
               diagnostics_before.write_received_count + 1U);
        assert(diagnostics_after.write_received_bytes ==
               diagnostics_before.write_received_bytes);
        assert(diagnostics_after.callback_error_count ==
               diagnostics_before.callback_error_count);
        assert(record.index == diagnostics_after.transaction_count);
        assert(record.direction == NRF_BOOT_TWIS_DIRECTION_WRITE_RECEIVED);
        assert(record.length == 0U && record.prefix_length == 0U);
        assert(record.result == 0);
        assert(read_log_status().next_seq == logs_before.next_seq);
    }

    assign = make_assign(10U);
    s_readdress_result = -5;
    send_assign(&assign);
    assert(s_submitted_work != NULL && s_submitted_work->queue != &k_sys_work_q);
    assert(s_submitted_work->queue->started);
    assert(strcmp(s_submitted_work->queue->name, "cs_hci_workqueue") == 0);
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
        uint8_t before_frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
        uint8_t *frame = read_frame(&read_length);
        const nrf_log_status_t logs_before = read_log_status();
        uint32_t after_length;

        assert(read_length == sizeof(before_frame));
        memcpy(before_frame, frame, read_length);
        s_callbacks->buf_write_received(NULL, NULL, 0U);
        frame = read_frame(&after_length);
        assert(after_length == read_length);
        assert(memcmp(before_frame, frame, after_length) == 0);
        assert(nrf_upstream_protocol_decode_status(
                   frame, NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH, &status) ==
               NRF_UPSTREAM_RESULT_OK);
        assert(status.request_id == 10U && status.result == NRF_UPSTREAM_RESULT_OK);
        assert(read_log_status().next_seq == logs_before.next_seq);
    }
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
        const nrf_upstream_sensor_refresh_t request = {.request_id = 0x55667788U};
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH];
        assert(nrf_upstream_protocol_encode_sensor_scan(&request, frame) == NRF_UPSTREAM_RESULT_OK);
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        assert(s_sensor_scan_calls == 1U && s_last_sensor_scan_id == request.request_id);
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
     * Queue pressure after a committed REMOTE assignment is recoverable.
     * Keep the node PENDING without reboot; retry activation on the HCI queue
     * and publish OK only when the sensor worker accepts its ACTIVATE event.
     */
    s_sensor_assignment_result = -ENOSPC;
    assign.request_id = 11U;
    send_assign(&assign);
    run_submitted_work();
    assert(s_sensor_assignment_calls == 2U);
    assert(s_reboot_calls == 0U);
    status = read_status(NULL);
    assert(status.request_id == 11U);
    assert(status.result == NRF_UPSTREAM_RESULT_PENDING);
    assert(status.role == NRF_UPSTREAM_ROLE_REMOTE);
    assert(s_delayed_work != NULL);
    /* The worker may remain congested for more than one retry. No reboot or
     * accidental OK is allowed while the queue still rejects activation. */
    run_delayed_work();
    assert(s_sensor_assignment_calls == 3U);
    assert(s_delayed_work != NULL);
    assert(s_reboot_calls == 0U);
    status = read_status(NULL);
    assert(status.request_id == 11U && status.result == NRF_UPSTREAM_RESULT_PENDING);
    {
        nrf_upstream_assign_t premature = assign;
        premature.request_id = 111U;
        send_assign(&premature);
        run_submitted_work();
        status = read_status(NULL);
        assert(status.request_id == 111U && status.result == NRF_UPSTREAM_RESULT_BUSY);
        assert(s_sensor_assignment_calls == 3U);
        assert(s_reboot_calls == 0U);
    }
    s_sensor_assignment_result = 0;
    run_delayed_work();
    assert(s_sensor_assignment_calls == 4U);
    status = read_status(NULL);
    assert(status.request_id == 11U && status.result == NRF_UPSTREAM_RESULT_OK);

    /* A later idempotent ASSIGN must still work after the deferred recovery. */
    /* If the optional CS radio failed at startup, REMOTE sensor assignment
     * must not reboot the nRF indefinitely. A real CS node conflict remains fatal. */
    s_sensor_assignment_result = 0;
    nrf_cs_pair_zephyr_stub_set_node_result = -EAGAIN;
    assign.request_id = 12U;
    send_assign(&assign);
    run_submitted_work();
    assert(s_sensor_assignment_calls == 5U);
    status = read_status(NULL);
    assert(s_reboot_calls == 0U);
    assert(status.request_id == 12U && status.result == NRF_UPSTREAM_RESULT_OK);
    nrf_cs_pair_zephyr_stub_set_node_result = 0;
    assert(read_log_status().next_seq == 2U);
    test_malformed_control_lengths_preserve_assignment();
    test_firmware_query_preserves_assignment();

    /* fail-before(real): a stopped shared queue reports INTERNAL_ERROR and drops that ASSIGN item. */
    {
        const unsigned before_sensor = s_sensor_assignment_calls;
        const unsigned before_submit = s_submit_calls;
        assign.request_id = 13U;
        nrf_hci_workqueue_test_forget_started();
        send_assign(&assign);
        status = read_status(NULL);
        assert(status.request_id == 13U && status.result == NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
        assert(s_submit_calls == before_submit);
        assert(s_submitted_work == NULL);

        nrf_hci_workqueue_start();
        assign.request_id = 14U;
        send_assign(&assign);
        assert(s_submitted_work != NULL && s_submitted_work->queue != &k_sys_work_q);
        assert(strcmp(s_submitted_work->queue->name, "cs_hci_workqueue") == 0);
        run_submitted_work();
        assert(s_sensor_assignment_calls == before_sensor + 1U);
        assert(s_sensor_assignment_request_ids[before_sensor] == 14U);
        status = read_status(NULL);
        assert(status.request_id == 14U && status.result == NRF_UPSTREAM_RESULT_OK);
    }


    {
        uint8_t invalid_frame[1] = {0U};
        s_callbacks->buf_write_received(NULL, invalid_frame, sizeof(invalid_frame));
        status = read_status(NULL);
        assert(status.result == NRF_UPSTREAM_RESULT_INVALID_FRAME);
    }

    {
        const unsigned before_sensor_assignment_calls = s_sensor_assignment_calls;
        for (i = 0U; i < 4U; ++i) {
            assign = make_assign(20U + i);
            send_assign(&assign);
            assign.request_id = 0xBADU;
            assign.node_id = 0x77U;
            assign.mux_channel = 0x66U;
            assign.role = NRF_UPSTREAM_ROLE_MAIN;
        }
        assign = make_assign(24U);
        send_assign(&assign);
        status = read_status(NULL);
        assert(status.request_id == 24U && status.result == NRF_UPSTREAM_RESULT_BUSY);
        run_submitted_work();
        status = read_status(NULL);
        assert(status.request_id == 23U && status.result == NRF_UPSTREAM_RESULT_OK);
        assert(s_readdress_calls == 2U);
        assert(s_sensor_assignment_calls == before_sensor_assignment_calls + 4U);
        for (i = 0U; i < 4U; ++i) {
            assert(s_sensor_assignment_request_ids[before_sensor_assignment_calls + i] == 20U + i);
            assert(s_sensor_assignments[before_sensor_assignment_calls + i].node_id == 2U);
            assert(s_sensor_assignments[before_sensor_assignment_calls + i].mux_channel == 1U);
            assert(s_sensor_assignments[before_sensor_assignment_calls + i].role == NRF_UPSTREAM_ROLE_REMOTE);
        }
    }

    /* Repeated queued ASSIGNs reuse one static worker and leave no pending work. */
    {
        const unsigned work_queue_start_calls = s_work_queue_start_calls;
        for (i = 0U; i < 32U; ++i) {
            assign = make_assign(100U + i);
            send_assign(&assign);
            run_submitted_work();
            assert(s_submitted_work == NULL);
            assert(s_work_queue_start_calls == work_queue_start_calls);
        }
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
        assert(s_submitted_work != NULL && s_submitted_work->queue == &k_sys_work_q);
        run_submitted_work();

        assert(nrf_cs_control_encode_status_request(&request, frame) == 0);
        s_callbacks->buf_write_received(NULL, frame, NRF_CS_CONTROL_STATUS_REQUEST_LENGTH);
        assert(nrf_log_protocol_encode_status_request(
                   &log_status_request,
                   log_request_frame) == NRF_LOG_ERROR_OK);
        s_callbacks->buf_write_received(NULL, log_request_frame, sizeof(log_request_frame));
        /* Two plain reads in a row (a retried first window) get the same selected reply, one window each. */
        assert(s_callbacks->buf_read_requested(NULL, &response_frame, &first_length) == 0);
        assert(nrf_bootstrap_runtime_zephyr_response_length() == NRF_LOG_PROTOCOL_STATUS_LENGTH);
        assert(first_length == NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES);
        memcpy(first_read, response_frame, first_length);
        assert(s_callbacks->buf_read_requested(NULL, &response_frame, &second_length) == 0);
        memcpy(second_read, response_frame, second_length);
        assert(second_length == first_length);
        assert(memcmp(first_read, second_read, first_length) == 0);
        assert(first_read[3] == NRF_LOG_MESSAGE_STATUS);

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

    {
        const nrf_cs_control_request_t duplicate = make_cs_request(0x521U, 0x522U);
        const nrf_cs_control_request_t busy = make_cs_request(0x523U, 0x524U);
        const nrf_cs_control_request_t stale = make_cs_request(0x525U, 0x526U);
        const unsigned execute_calls_before = nrf_cs_pair_zephyr_stub_execute_calls;
        nrf_cs_control_result_t result;

        assert(s_cs_execute_queue.max_msgs == 2U && s_cs_execute_queue.count == 0U);
        send_cs_execute(&duplicate);
        assert(s_cs_execute_queue.count == 1U);
        send_cs_execute(&duplicate);
        assert(s_cs_execute_queue.count == 2U);
        send_cs_execute(&busy);
        assert(s_cs_execute_queue.count == 2U);

        result = read_cs_result_for_request(&busy);
        assert(result.request_id == busy.request_id);
        assert(result.command_result == NRF_CS_CONTROL_RESULT_BUSY);
        run_submitted_work();
        assert(s_cs_execute_queue.count == 0U);
        assert(nrf_cs_pair_zephyr_stub_execute_calls == execute_calls_before + 1U);

        result = read_cs_result_for_request(&duplicate);
        assert(result.request_id == duplicate.request_id);
        assert(result.state == NRF_CS_CONTROL_STATE_COMPLETE);
        result = read_cs_result_for_request(&stale);
        assert(result.request_id == stale.request_id);
        assert(result.command_result == NRF_CS_CONTROL_RESULT_STALE);
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
    nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis_diagnostics);
    {
        uint8_t unknown_request[4] = {0U, 0U, 0U, 0xA5U};
        const uint32_t previous_write_count = twis_diagnostics.write_received_count;
        const uint32_t previous_write_bytes = twis_diagnostics.write_received_bytes;
        const uint32_t previous_read_count = twis_diagnostics.read_requested_count;
        const uint32_t previous_error_count = twis_diagnostics.callback_error_count;

        s_callbacks->buf_write_received(
            NULL, unknown_request, (uint32_t)sizeof(unknown_request));
        s_callbacks->buf_write_received(
            NULL, unknown_request, (uint32_t)sizeof(unknown_request));
        assert(s_callbacks->buf_read_requested(NULL, NULL, &read_length) == -EINVAL);
        nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis_diagnostics);
        assert(twis_diagnostics.write_received_count == previous_write_count + 2U);
        assert(twis_diagnostics.write_received_bytes == previous_write_bytes + 8U);
        assert(twis_diagnostics.read_requested_count == previous_read_count + 1U);
        /* Both unsupported writes and the failed read response are counted. */
        assert(twis_diagnostics.callback_error_count == previous_error_count + 3U);
        assert(twis_diagnostics.last_request_type_valid);
        assert(twis_diagnostics.last_request_type == 0xA5U);
        assert(twis_diagnostics.last_request_type_count == 2U);
    }
    assert(s_reboot_calls == 0U);
    {
        nrf_boot_twis_record_t record;
        while (nrf_bootstrap_runtime_zephyr_drain_twis_record(&record)) {}
        const uint32_t callbacks_before = twis_diagnostics.transaction_count;
        const nrf_boot_twis_record_t hardware = {
            .direction = NRF_BOOT_TWIS_DIRECTION_READ_ERROR,
            .length = 38U,
            .prefix_length = 4U,
            .prefix = {0x47U, 0x53U, 0x01U, 0x82U},
            .tx_amount = 32U,
            .hw_error = 4U,
            .tx_prepared = true,
            .result = -EIO,
        };
        nrf_bootstrap_runtime_zephyr_record_twis_hardware(NULL);
        nrf_bootstrap_runtime_zephyr_record_twis_hardware(&hardware);
        assert(nrf_bootstrap_runtime_zephyr_drain_twis_record(&record));
        assert(record.index == callbacks_before);
        assert(record.direction == NRF_BOOT_TWIS_DIRECTION_READ_ERROR);
        assert(record.length == 38U && record.prefix[3] == 0x82U);
        assert(record.tx_amount == 32U && record.hw_error == 4U && record.tx_prepared);
        assert(!nrf_bootstrap_runtime_zephyr_drain_twis_record(&record));
        nrf_bootstrap_runtime_zephyr_get_twis_diagnostics(&twis_diagnostics);
        assert(twis_diagnostics.transaction_count == callbacks_before);
    }
    test_twis_log_contract_never_leaves_the_target_without_a_response();
    test_twis_read_windows_continue_the_selected_response();
    test_twis_read_window_requests_are_validated_and_one_shot();
    test_autonomous_cs_cycle_messages(NRF_CS_OUTCOME_SUCCESS);
    test_autonomous_cs_cycle_messages(NRF_CS_OUTCOME_TIMEOUT);
    test_autonomous_cs_cycle_messages(NRF_CS_OUTCOME_TRANSPORT_ERROR);
    test_relay_hooks_reach_the_cycle_runtime();
    {
        const nrf_upstream_sensor_refresh_t request = {.request_id = 777U};
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH];
        s_sensor_scan_result = -EBUSY;
        assert(nrf_upstream_protocol_encode_sensor_scan(&request, frame) == NRF_UPSTREAM_RESULT_OK);
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        const nrf_log_record_t rejected = find_log_event(NRF_LOG_EVENT_SENSOR_SCAN_DONE);
        assert(rejected.arg0 == 777U && rejected.arg1 == (uint32_t)-EBUSY);
        s_sensor_scan_result = 0;
    }
    {
        const nrf_upstream_sensor_refresh_t request = {.request_id = 19U};
        uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH];
        const unsigned scans_before = s_sensor_scan_calls;
        assert(nrf_upstream_protocol_encode_sensor_scan(&request, frame) == NRF_UPSTREAM_RESULT_OK);
        frame[6] ^= 1U;
        s_callbacks->buf_write_received(NULL, frame, sizeof(frame));
        assert(s_sensor_scan_calls == scans_before);
    }
    /* Only an unavailable optional CS subsystem is recoverable. A conflicting
     * CS identity is still a fail-closed assignment invariant. */
    {
        const unsigned reboots_before = s_reboot_calls;
        assign.request_id = 0x314159U;
        nrf_cs_pair_zephyr_stub_set_node_result = -EALREADY;
        send_assign(&assign);
        run_submitted_work();
        status = read_status(NULL);
        assert(status.request_id == assign.request_id);
        assert(status.result == NRF_UPSTREAM_RESULT_INTERNAL_ERROR);
        assert(s_reboot_calls == reboots_before + 1U);
        assert(s_reboot_intent_count != 0U);
        assert(s_reboot_intent_stage == NRF_REBOOT_INTENT_ASSIGN_CS_PAIR);
        assert(s_reboot_intent_error == -EALREADY);
        nrf_cs_pair_zephyr_stub_set_node_result = 0;
    }
#if defined(NRF_TWIS_RUNTIME_INTEGRATION)
    test_address_only_status_select_with_stale_dma_count();
#endif
    return 0;
}
