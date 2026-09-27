#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "nrf_bootstrap_runtime_zephyr.h"
#include "nrf_sensor_upstream_runtime_zephyr.h"
#include "nrf_upstream_protocol.h"
#include "nrf_upstream_target_zephyr.h"

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

static nrf_upstream_status_t read_status(uint32_t *read_length)
{
    uint8_t *frame = NULL;
    uint32_t length = 0U;
    nrf_upstream_status_t status;
    assert(s_callbacks != NULL);
    assert(s_callbacks->buf_read_requested(NULL, &frame, &length) == 0);
    assert(length == NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH ||
           length == NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    assert(nrf_upstream_protocol_decode_status(
               frame,
               NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH,
               &status) == NRF_UPSTREAM_RESULT_OK);
    if (read_length != NULL) {
        *read_length = length;
    }
    return status;
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

int main(void)
{
    nrf_node_state_t node_state;
    nrf_upstream_status_t status;
    nrf_upstream_assign_t assign;
    unsigned i;
    uint32_t read_length;

    memset(&s_sensor_snapshot, 0, sizeof(s_sensor_snapshot));
    s_sensor_snapshot.result = NRF_UPSTREAM_SENSOR_RESULT_NOT_READY;

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

    assert(s_reboot_calls == 1U);
    return 0;
}
