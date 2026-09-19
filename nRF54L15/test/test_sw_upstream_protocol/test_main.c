#include <stdint.h>
#include <string.h>

#include <unity.h>

#include "nrf_upstream_protocol.h"

/* The protocol implementation is dependency-free and is intentionally
 * compiled into this native PlatformIO suite, not copied into the test. */
#include "../../src/nrf_upstream_protocol.c"

static void test_remote_snapshot_is_112_bytes_and_round_trips(void)
{
    nrf_upstream_status_t status;
    nrf_upstream_sensor_snapshot_t sensor;
    nrf_upstream_status_t decoded_status;
    nrf_upstream_sensor_snapshot_t decoded_sensor;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

    memset(&status, 0, sizeof(status));
    status.request_id = 0x11223344U;
    status.role = NRF_UPSTREAM_ROLE_REMOTE;
    status.node_id = 1U;
    status.mux_channel = 0U;
    status.target_address = NRF_UPSTREAM_ADDRESS_REMOTE;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    status.flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED |
                   NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;

    memset(&sensor, 0, sizeof(sensor));
    sensor.request_id = status.request_id;
    sensor.sequence = 7U;
    sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_MASK;
    sensor.result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    sensor.flags = NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED |
                   NRF_UPSTREAM_SENSOR_FLAG_IMU_READY |
                   NRF_UPSTREAM_SENSOR_FLAG_ENV_READY |
                   NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
    sensor.pressure_pa = 101205U;
    sensor.altitude_mm = 10000;
    sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
    sensor.orientation_tilt_sin2_ppm = 12345U;

    TEST_ASSERT_EQUAL(
        NRF_UPSTREAM_RESULT_OK,
        nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame));
    TEST_ASSERT_EQUAL_UINT16(112U, frame[4] | ((uint16_t)frame[5] << 8U));
    TEST_ASSERT_EQUAL(
        NRF_UPSTREAM_RESULT_OK,
        nrf_upstream_protocol_decode_remote_snapshot(
            frame, sizeof(frame), &decoded_status, &decoded_sensor));
    TEST_ASSERT_EQUAL_UINT32(sensor.request_id, decoded_sensor.request_id);
    TEST_ASSERT_EQUAL_INT32(sensor.altitude_mm, decoded_sensor.altitude_mm);
    TEST_ASSERT_EQUAL_UINT8(sensor.orientation_state, decoded_sensor.orientation_state);
    TEST_ASSERT_EQUAL_UINT32(
        sensor.orientation_tilt_sin2_ppm, decoded_sensor.orientation_tilt_sin2_ppm);
}

static void test_remote_snapshot_rejects_wrong_wire_length(void)
{
    nrf_upstream_status_t status;
    nrf_upstream_sensor_snapshot_t sensor;
    nrf_upstream_status_t decoded_status;
    nrf_upstream_sensor_snapshot_t decoded_sensor;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

    memset(&status, 0, sizeof(status));
    status.role = NRF_UPSTREAM_ROLE_REMOTE;
    status.node_id = 1U;
    status.mux_channel = 0U;
    status.target_address = NRF_UPSTREAM_ADDRESS_REMOTE;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    memset(&sensor, 0, sizeof(sensor));
    sensor.request_id = status.request_id;

    TEST_ASSERT_EQUAL(
        NRF_UPSTREAM_RESULT_OK,
        nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame));
    TEST_ASSERT_EQUAL(
        NRF_UPSTREAM_RESULT_INVALID_FRAME,
        nrf_upstream_protocol_decode_remote_snapshot(
            frame, 102U, &decoded_status, &decoded_sensor));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_remote_snapshot_is_112_bytes_and_round_trips);
    RUN_TEST(test_remote_snapshot_rejects_wrong_wire_length);
    return UNITY_END();
}
