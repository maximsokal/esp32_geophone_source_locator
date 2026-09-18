#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_orientation.h"
#include "nrf_upstream_protocol.h"

static nrf_upstream_assign_t make_assign(void)
{
    nrf_upstream_assign_t assign;
    memset(&assign, 0, sizeof(assign));
    assign.request_id = 0x78563412U;
    assign.hardware_id_length = 4U;
    assign.hardware_id[0] = 0xAAU;
    assign.hardware_id[1] = 0xBBU;
    assign.hardware_id[2] = 0xCCU;
    assign.hardware_id[3] = 0xDDU;
    assign.role = NRF_UPSTREAM_ROLE_REMOTE;
    assign.node_id = 3U;
    assign.mux_channel = 2U;
    return assign;
}

static nrf_upstream_status_t make_remote_status(void)
{
    nrf_upstream_status_t status;
    memset(&status, 0, sizeof(status));
    status.request_id = 0x04030201U;
    status.hardware_id_length = 4U;
    status.hardware_id[0] = 1U;
    status.hardware_id[1] = 2U;
    status.hardware_id[2] = 3U;
    status.hardware_id[3] = 4U;
    status.role = NRF_UPSTREAM_ROLE_REMOTE;
    status.node_id = 4U;
    status.mux_channel = 3U;
    status.target_address = NRF_UPSTREAM_ADDRESS_REMOTE;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    status.flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED |
                   NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;
    return status;
}

static nrf_upstream_sensor_snapshot_t make_sensor_snapshot(void)
{
    nrf_upstream_sensor_snapshot_t sensor;
    memset(&sensor, 0, sizeof(sensor));
    sensor.request_id = 0x44332211U;
    sensor.sequence = 9U;
    sensor.valid_mask = NRF_UPSTREAM_SENSOR_VALID_MASK;
    sensor.sample_complete_uptime_ms = UINT64_C(0x0807060504030201);
    sensor.result = NRF_UPSTREAM_SENSOR_RESULT_OK;
    sensor.flags = NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED |
                   NRF_UPSTREAM_SENSOR_FLAG_IMU_READY |
                   NRF_UPSTREAM_SENSOR_FLAG_ENV_READY |
                   NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE;
    sensor.accel_mg[0] = -1001;
    sensor.accel_mg[1] = 2;
    sensor.accel_mg[2] = 1003;
    sensor.gyro_mdps[0] = INT32_MIN;
    sensor.gyro_mdps[1] = 0;
    sensor.gyro_mdps[2] = INT32_MAX;
    sensor.imu_temperature_milli_c = -12345;
    sensor.env_temperature_milli_c = 23456;
    sensor.pressure_pa = 100123U;
    sensor.humidity_milli_percent = 56789U;
    sensor.altitude_mm = -9876;
    sensor.orientation_state = 2U; /* NRF_ORIENTATION_STATE_ACCEPTED */
    sensor.orientation_blink_class = 0U; /* NRF_ORIENTATION_BLINK_NONE */
    sensor.orientation_tilt_sin2_ppm = 12345U;
    return sensor;
}

static void refresh_assign_crc(uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH])
{
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 22U);
    frame[22] = (uint8_t)(crc & 0xFFU);
    frame[23] = (uint8_t)(crc >> 8U);
}

static void refresh_sensor_request_crc(uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH])
{
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 10U);
    frame[10] = (uint8_t)(crc & 0xFFU);
    frame[11] = (uint8_t)(crc >> 8U);
}

static void refresh_remote_snapshot_crc(uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH])
{
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 110U);
    frame[110] = (uint8_t)(crc & 0xFFU);
    frame[111] = (uint8_t)(crc >> 8U);
}

static void test_crc_known_vector(void)
{
    static const uint8_t text[] = "123456789";
    assert(nrf_upstream_protocol_crc16_ccitt_false(text, 9U) == 0x29B1U);
}

static void test_assign_exact_layout_and_roundtrip(void)
{
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_assign_t decoded;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    assert(frame[0] == 0x47U && frame[1] == 0x53U);
    assert(frame[2] == 1U && frame[3] == 0x01U);
    assert(frame[4] == 24U && frame[5] == 0U);
    assert(frame[6] == 0x12U && frame[7] == 0x34U && frame[8] == 0x56U && frame[9] == 0x78U);
    assert(frame[10] == 4U);
    assert(memcmp(&frame[11], assign.hardware_id, 4U) == 0);
    assert(frame[15] == 0U && frame[18] == 0U);
    assert(frame[19] == NRF_UPSTREAM_ROLE_REMOTE);
    assert(frame[20] == 3U && frame[21] == 2U);
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_OK);
    assert(memcmp(&assign, &decoded, sizeof(assign)) == 0);
    assert(nrf_upstream_protocol_request_id_hint(frame, sizeof(frame)) == assign.request_id);
}

static void test_assign_rejects_corruption_and_header_errors(void)
{
    nrf_upstream_assign_t assign = make_assign();
    nrf_upstream_assign_t decoded;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[20] ^= 1U;
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_BAD_CRC);

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[2] = 2U;
    refresh_assign_crc(frame);
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION);

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[3] = 0x44U;
    refresh_assign_crc(frame);
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE);

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[4] = 23U;
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_INVALID_FRAME);
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame) - 1U, &decoded) == NRF_UPSTREAM_RESULT_INVALID_FRAME);
}

static void test_assign_field_validation(void)
{
    nrf_upstream_assign_t assign = make_assign();
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH];

    assign.request_id = 0U;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID);
    assign = make_assign();
    assign.role = NRF_UPSTREAM_ROLE_UNASSIGNED;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_ROLE_INVALID);
    assign = make_assign();
    assign.node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_NODE_ID_INVALID);
    assign = make_assign();
    assign.role = NRF_UPSTREAM_ROLE_MAIN;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_CHANNEL_INVALID);
    assign.mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    assign = make_assign();
    assign.mux_channel = 8U;
    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_CHANNEL_INVALID);
}

static void test_status_exact_layout(void)
{
    nrf_upstream_status_t status;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH];
    nrf_upstream_status_t decoded;
    memset(&status, 0, sizeof(status));
    status.request_id = 0x04030201U;
    status.hardware_id_length = 3U;
    status.hardware_id[0] = 1U;
    status.hardware_id[1] = 2U;
    status.hardware_id[2] = 3U;
    status.role = NRF_UPSTREAM_ROLE_MAIN;
    status.node_id = 7U;
    status.mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
    status.target_address = NRF_UPSTREAM_ADDRESS_MAIN;
    status.lifecycle = NRF_UPSTREAM_LIFECYCLE_READY;
    status.result = NRF_UPSTREAM_RESULT_OK;
    status.flags = NRF_UPSTREAM_STATUS_FLAG_ASSIGNED | NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED;

    assert(nrf_upstream_protocol_encode_status(&status, frame) == NRF_UPSTREAM_RESULT_OK);
    assert(frame[0] == 0x47U && frame[1] == 0x53U && frame[3] == 0x80U);
    assert(frame[4] == 28U && frame[5] == 0U);
    assert(frame[6] == 1U && frame[7] == 2U && frame[8] == 3U && frame[9] == 4U);
    assert(frame[10] == 3U && frame[11] == 1U && frame[12] == 2U && frame[13] == 3U);
    assert(frame[19] == NRF_UPSTREAM_ROLE_MAIN && frame[20] == 7U && frame[21] == 0xFFU);
    assert(frame[22] == 0x31U && frame[23] == 1U && frame[24] == 0U && frame[25] == 3U);
    assert(((uint16_t)frame[26] | ((uint16_t)frame[27] << 8U)) ==
           nrf_upstream_protocol_crc16_ccitt_false(frame, 26U));
    assert(nrf_upstream_protocol_decode_status(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_OK);
    assert(memcmp(&status, &decoded, sizeof(status)) == 0);
}

static void test_sensor_refresh_layout_and_validation(void)
{
    nrf_upstream_sensor_refresh_t request = {.request_id = 0x78563412U};
    nrf_upstream_sensor_refresh_t decoded = {0};
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH];

    assert(nrf_upstream_protocol_encode_sensor_refresh(&request, frame) == NRF_UPSTREAM_RESULT_OK);
    assert(frame[0] == 0x47U && frame[1] == 0x53U && frame[2] == 1U && frame[3] == 0x02U);
    assert(frame[4] == 12U && frame[5] == 0U);
    assert(frame[6] == 0x12U && frame[7] == 0x34U && frame[8] == 0x56U && frame[9] == 0x78U);
    assert(nrf_upstream_protocol_decode_sensor_refresh(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_OK);
    assert(decoded.request_id == request.request_id);

    frame[6] ^= 1U;
    assert(nrf_upstream_protocol_decode_sensor_refresh(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_BAD_CRC);

    assert(nrf_upstream_protocol_encode_sensor_refresh(&request, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[3] = 0x77U;
    refresh_sensor_request_crc(frame);
    assert(nrf_upstream_protocol_decode_sensor_refresh(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE);

    request.request_id = 0U;
    assert(nrf_upstream_protocol_encode_sensor_refresh(&request, frame) == NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID);
}

static void test_remote_snapshot_prefix_and_roundtrip(void)
{
    nrf_upstream_status_t status = make_remote_status();
    nrf_upstream_sensor_snapshot_t sensor = make_sensor_snapshot();
    nrf_upstream_status_t decoded_status;
    nrf_upstream_sensor_snapshot_t decoded_sensor;
    uint8_t status_frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH];
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

    assert(nrf_upstream_protocol_encode_status(&status, status_frame) == NRF_UPSTREAM_RESULT_OK);
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_OK);
    assert(memcmp(frame, status_frame, sizeof(status_frame)) == 0);
    assert(frame[28] == 0x53U && frame[29] == 0x4EU);
    assert(frame[30] == 1U && frame[31] == 1U);
    assert(frame[32] == 112U && frame[33] == 0U);
    assert(frame[34] == 0x11U && frame[35] == 0x22U && frame[36] == 0x33U && frame[37] == 0x44U);
    assert(frame[50] == 0x01U && frame[57] == 0x08U);
    /* MAX-114 extension: signed altitude, state/blink enums and tilt metric
     * have fixed little-endian offsets after the legacy 100-byte payload. */
    assert(frame[100] == 0x6CU && frame[101] == 0xD9U &&
           frame[102] == 0xFFU && frame[103] == 0xFFU);
    assert(frame[104] == NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED);
    assert(frame[105] == NRF_ORIENTATION_BLINK_NONE);
    assert(frame[106] == 0x39U && frame[107] == 0x30U &&
           frame[108] == 0x00U && frame[109] == 0x00U);
    assert(((uint16_t)frame[110] | ((uint16_t)frame[111] << 8U)) ==
           nrf_upstream_protocol_crc16_ccitt_false(frame, 110U));
    assert(nrf_upstream_protocol_decode_status(frame, NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH, &decoded_status) == NRF_UPSTREAM_RESULT_OK);
    assert(memcmp(&status, &decoded_status, sizeof(status)) == 0);
    assert(nrf_upstream_protocol_decode_remote_snapshot(frame, sizeof(frame), &decoded_status, &decoded_sensor) == NRF_UPSTREAM_RESULT_OK);
    assert(memcmp(&status, &decoded_status, sizeof(status)) == 0);
    assert(memcmp(&sensor, &decoded_sensor, sizeof(sensor)) == 0);
}

static void test_remote_snapshot_corruption_and_validation(void)
{
    nrf_upstream_status_t status = make_remote_status();
    nrf_upstream_sensor_snapshot_t sensor = make_sensor_snapshot();
    nrf_upstream_status_t decoded_status;
    nrf_upstream_sensor_snapshot_t decoded_sensor;
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];

    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[99] ^= 1U;
    assert(nrf_upstream_protocol_decode_remote_snapshot(frame, sizeof(frame), &decoded_status, &decoded_sensor) == NRF_UPSTREAM_RESULT_BAD_CRC);

    sensor.flags |= 0x80U;
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    sensor = make_sensor_snapshot();
    sensor.flags &= (uint8_t)(
        NRF_UPSTREAM_SENSOR_FLAG_MASK ^
        NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE
    );
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    sensor = make_sensor_snapshot();
    sensor.fault_mask = NRF_UPSTREAM_SENSOR_VALID_IMU;
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    sensor = make_sensor_snapshot();
    sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_MAX + 1U;
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    sensor = make_sensor_snapshot();
    sensor.orientation_blink_class = NRF_UPSTREAM_ORIENTATION_BLINK_MAX + 1U;
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_INVALID_FRAME);

    sensor = make_sensor_snapshot();
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[104] = NRF_UPSTREAM_ORIENTATION_STATE_MAX + 1U;
    refresh_remote_snapshot_crc(frame);
    assert(nrf_upstream_protocol_decode_remote_snapshot(
               frame, sizeof(frame), &decoded_status, &decoded_sensor) ==
           NRF_UPSTREAM_RESULT_INVALID_FRAME);
    assert(decoded_sensor.request_id == 0U && decoded_sensor.valid_mask == 0U);

    sensor = make_sensor_snapshot();
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[105] = NRF_UPSTREAM_ORIENTATION_BLINK_MAX + 1U;
    refresh_remote_snapshot_crc(frame);
    assert(nrf_upstream_protocol_decode_remote_snapshot(
               frame, sizeof(frame), &decoded_status, &decoded_sensor) ==
           NRF_UPSTREAM_RESULT_INVALID_FRAME);
    assert(decoded_sensor.request_id == 0U && decoded_sensor.valid_mask == 0U);
}

static void test_orientation_wire_constants_match_estimator_enums(void)
{
    /*
     * nrf_upstream_protocol.h intentionally duplicates these numeric values
     * instead of including nrf_orientation.h (kept dependency-free for the
     * ESP32 build). This test is the drift guard: if nrf_orientation_state_t
     * or nrf_orientation_blink_class_t's ordering ever changes, this must
     * be updated in lockstep or ESP32's leveling-gate check silently breaks.
     */
    assert((uint8_t)NRF_ORIENTATION_STATE_UNAVAILABLE == 0U);
    assert((uint8_t)NRF_ORIENTATION_STATE_MOVING == 1U);
    assert((uint8_t)NRF_ORIENTATION_STATE_ACCEPTED ==
           NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED);
    assert((uint8_t)NRF_ORIENTATION_STATE_OUTSIDE ==
           NRF_UPSTREAM_ORIENTATION_STATE_MAX);
    assert((uint8_t)NRF_ORIENTATION_BLINK_NONE == 0U);
    assert((uint8_t)NRF_ORIENTATION_BLINK_SLOW == 1U);
    assert((uint8_t)NRF_ORIENTATION_BLINK_MEDIUM == 2U);
    assert((uint8_t)NRF_ORIENTATION_BLINK_FAST ==
           NRF_UPSTREAM_ORIENTATION_BLINK_MAX);
}

static void test_role_address_contract(void)
{
    assert(nrf_upstream_protocol_address_for_role(NRF_UPSTREAM_ROLE_MAIN) == 0x31U);
    assert(nrf_upstream_protocol_address_for_role(NRF_UPSTREAM_ROLE_REMOTE) == 0x32U);
    assert(nrf_upstream_protocol_address_for_role(NRF_UPSTREAM_ROLE_UNASSIGNED) == 0U);
}

int main(void)
{
    test_crc_known_vector();
    test_assign_exact_layout_and_roundtrip();
    test_assign_rejects_corruption_and_header_errors();
    test_assign_field_validation();
    test_status_exact_layout();
    test_sensor_refresh_layout_and_validation();
    test_remote_snapshot_prefix_and_roundtrip();
    test_remote_snapshot_corruption_and_validation();
    test_orientation_wire_constants_match_estimator_enums();
    test_role_address_contract();
    return 0;
}
