#define main upstream_baseline_main
#include "nrf_upstream_protocol_test.c"
#undef main

#include <stdbool.h>
#include <stdio.h>

static void test_all_state_and_command_bytes(void)
{
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        for (unsigned field = 0U; field < 4U; ++field) {
            nrf_upstream_status_t status = make_remote_status();
            nrf_upstream_sensor_snapshot_t sensor = make_sensor_snapshot();
            bool valid;
            if (field == 0U) {
                sensor.orientation_state = (uint8_t)byte;
                valid = byte <= 3U;
            } else if (field == 1U) {
                sensor.orientation_blink_class = (uint8_t)byte;
                valid = byte <= 3U;
            } else if (field == 2U) {
                sensor.reference_state = (uint8_t)byte;
                sensor.relative_z_mm = 0;
                valid = byte <= 3U;
            } else {
                sensor.imu_cal_state = (uint8_t)byte;
                valid = byte <= 4U;
            }
            assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) ==
                (valid ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME));
        }
        for (unsigned field = 0U; field < 6U; ++field) {
            nrf_upstream_status_t status = make_remote_status();
            bool valid;
            if (field == 0U) {
                status.role = (uint8_t)byte;
                if (byte != 2U) status.mux_channel = NRF_UPSTREAM_MUX_CHANNEL_NONE;
                if (byte == 0U) status.node_id = NRF_UPSTREAM_NODE_ID_UNASSIGNED;
                valid = byte <= 2U;
            } else if (field == 1U) {
                status.lifecycle = (uint8_t)byte;
                valid = byte <= 2U;
            } else if (field == 2U) {
                status.result = (uint8_t)byte;
                valid = byte <= 15U;
            } else if (field == 3U) {
                status.flags = (uint8_t)byte;
                valid = byte <= 3U;
            } else if (field == 4U) {
                status.target_address = (uint8_t)byte;
                valid = byte <= 127U;
            } else {
                status.mux_channel = (uint8_t)byte;
                valid = byte <= 7U;
            }
            const int result = nrf_upstream_protocol_encode_status(&status, frame);
            assert(result == (valid ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME));
            if (valid) {
                nrf_upstream_status_t decoded;
                assert(nrf_upstream_protocol_decode_status(frame,
                    NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH, &decoded) == NRF_UPSTREAM_RESULT_OK);
                assert(decoded.role == status.role && decoded.lifecycle == status.lifecycle &&
                    decoded.result == status.result && decoded.flags == status.flags &&
                    decoded.target_address == status.target_address && decoded.mux_channel == status.mux_channel);
            }
        }
        const nrf_upstream_height_reference_t height = {.action = (uint8_t)byte,
            .request_id = UINT32_MAX, .generation = UINT16_MAX};
        const nrf_upstream_imu_calibrate_t imu = {.action = (uint8_t)byte,
            .request_id = UINT32_MAX, .generation = UINT16_MAX};
        const int expected = byte == 1U || byte == 2U ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME;
        assert(nrf_upstream_protocol_encode_height_reference(&height, frame) == expected);
        assert(nrf_upstream_protocol_encode_imu_calibrate(&imu, frame) == expected);
    }
    puts("SIGNAL_CASE UPSTREAM.STATES_COMMANDS PASS bytes=256 fields=12");
}

static void test_upstream_enum_and_mask_domains(void)
{
    nrf_upstream_status_t status = make_remote_status();
    nrf_upstream_sensor_snapshot_t sensor = make_sensor_snapshot();
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    for (unsigned byte = 0U; byte <= UINT8_MAX; ++byte) {
        sensor = make_sensor_snapshot();
        sensor.result = (uint8_t)byte;
        assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) ==
            (byte <= NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME));
        sensor = make_sensor_snapshot();
        sensor.flags = (uint8_t)byte;
        const bool flags_valid = (byte & ~NRF_UPSTREAM_SENSOR_FLAG_MASK) == 0U &&
            (byte & NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE) != 0U;
        assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) ==
            (flags_valid ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME));
        for (unsigned fault = 0U; fault <= UINT8_MAX; ++fault) {
            sensor = make_sensor_snapshot();
            sensor.valid_mask = byte;
            sensor.fault_mask = fault;
            if (byte == 0U) sensor.flags = (uint8_t)(sensor.flags & ~NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE);
            const bool coherent = ((byte | fault) & ~3U) == 0U && (byte & fault) == 0U;
            assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, frame) ==
                (coherent ? NRF_UPSTREAM_RESULT_OK : NRF_UPSTREAM_RESULT_INVALID_FRAME));
        }
    }
    puts("SIGNAL_CASE UPSTREAM.ENUMS_MASKS PASS byte_pairs=65536");
}

static void test_every_snapshot_bit_and_truncated_length(void)
{
    nrf_upstream_status_t status = make_remote_status(), decoded_status;
    nrf_upstream_sensor_snapshot_t sensor = make_sensor_snapshot(), decoded_sensor;
    uint8_t valid[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH];
    assert(nrf_upstream_protocol_encode_remote_snapshot(&status, &sensor, valid) == NRF_UPSTREAM_RESULT_OK);
    for (size_t bit = 0U; bit < sizeof(valid) * 8U; ++bit) {
        uint8_t corrupt[sizeof(valid)];
        memcpy(corrupt, valid, sizeof(valid));
        corrupt[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        assert(nrf_upstream_protocol_decode_remote_snapshot(corrupt, sizeof(corrupt),
            &decoded_status, &decoded_sensor) != NRF_UPSTREAM_RESULT_OK);
    }
    for (size_t length = 0U; length < sizeof(valid); ++length) {
        assert(nrf_upstream_protocol_decode_remote_snapshot(valid, length,
            &decoded_status, &decoded_sensor) != NRF_UPSTREAM_RESULT_OK);
    }
    puts("SIGNAL_CASE UPSTREAM.CRC_LENGTH PASS bits=1088 lengths=136");
}

int main(void)
{
    assert(upstream_baseline_main() == 0);
    test_upstream_enum_and_mask_domains();
    test_every_snapshot_bit_and_truncated_length();
    test_all_state_and_command_bytes();
    return 0;
}
