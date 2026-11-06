#ifndef NRF_UPSTREAM_PROTOCOL_H
#define NRF_UPSTREAM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_UPSTREAM_PROTOCOL_VERSION 1U
#define NRF_UPSTREAM_PROTOCOL_HW_ID_MAX 8U
#define NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH 24U
#define NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH 28U
#define NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH 12U
#define NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH 12U
#define NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH 16U
#define NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH 20U
#define NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH 12U
/*
 * Longest read the ESP32 master performs as one FIFO transfer. A longer read is
 * split into segments with the master holding SCL low between them, and on the
 * wire capture (2026-10-26) every response longer than this arrived damaged in
 * its tail while 259 of 259 status reads of 28 bytes were intact. Responses
 * above this size are therefore read as consecutive windows of at most this
 * many bytes (see nrf_read_window.h).
 */
#define NRF_UPSTREAM_PROTOCOL_READ_WINDOW_BYTES 28U
#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH 112U
#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V2_LENGTH 120U
#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH 136U
#define NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V3_LENGTH

#define NRF_UPSTREAM_ADDRESS_DISCOVERY 0x30U
#define NRF_UPSTREAM_ADDRESS_MAIN      0x31U
#define NRF_UPSTREAM_ADDRESS_REMOTE    0x32U

#define NRF_UPSTREAM_NODE_ID_UNASSIGNED 0xFFU
#define NRF_UPSTREAM_MUX_CHANNEL_NONE   0xFFU

#define NRF_UPSTREAM_STATUS_FLAG_ASSIGNED          0x01U
#define NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED 0x02U

#define NRF_UPSTREAM_SENSOR_FLAG_PREPARE_ATTEMPTED 0x01U
#define NRF_UPSTREAM_SENSOR_FLAG_IMU_READY         0x02U
#define NRF_UPSTREAM_SENSOR_FLAG_ENV_READY         0x04U
#define NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE  0x08U
#define NRF_UPSTREAM_SENSOR_FLAG_FILTERED          0x10U
#define NRF_UPSTREAM_SENSOR_FLAG_MASK              0x1FU

#define NRF_UPSTREAM_SENSOR_VALID_IMU 0x01U
#define NRF_UPSTREAM_SENSOR_VALID_ENV 0x02U
#define NRF_UPSTREAM_SENSOR_VALID_MASK 0x03U

typedef enum {
    NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED = 0,
    NRF_UPSTREAM_REFERENCE_VALID = 1,
    NRF_UPSTREAM_REFERENCE_REJECTED_MOVING = 2,
    NRF_UPSTREAM_REFERENCE_SENSOR_INVALID = 3,
} nrf_upstream_reference_state_t;

typedef enum {
    NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE = 1,
    NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR = 2,
} nrf_upstream_height_reference_action_t;

typedef enum {
    NRF_UPSTREAM_IMU_CALIBRATION_NOT_CALIBRATED = 0,
    NRF_UPSTREAM_IMU_CALIBRATION_CALIBRATED = 1,
    NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_MOVING = 2,
    NRF_UPSTREAM_IMU_CALIBRATION_REJECTED_GRAVITY = 3,
    NRF_UPSTREAM_IMU_CALIBRATION_SENSOR_INVALID = 4,
} nrf_upstream_imu_calibration_state_t;

typedef enum {
    NRF_UPSTREAM_IMU_CALIBRATE_CAPTURE = 1,
    NRF_UPSTREAM_IMU_CALIBRATE_CLEAR = 2,
} nrf_upstream_imu_calibrate_action_t;

/*
 * Numeric range mirrors nrf_orientation_state_t/nrf_orientation_blink_class_t
 * (UNAVAILABLE/MOVING/ACCEPTED/OUTSIDE and NONE/SLOW/MEDIUM/FAST, both 0..3).
 * This header intentionally does not include nrf_orientation.h to stay a
 * dependency-free wire-format definition shared as-is by the ESP32 build.
 */
#define NRF_UPSTREAM_ORIENTATION_STATE_MAX 3U
#define NRF_UPSTREAM_ORIENTATION_BLINK_MAX 3U

/* Mirror nrf_orientation_state_t values used by the ESP32. */
#define NRF_UPSTREAM_ORIENTATION_STATE_MOVING 1U
#define NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED 2U

typedef enum {
    NRF_UPSTREAM_MESSAGE_ASSIGN = 0x01,
    NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH = 0x02,
    NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE = 0x05,
    NRF_UPSTREAM_MESSAGE_IMU_CALIBRATE = 0x06,
    NRF_UPSTREAM_MESSAGE_READ_WINDOW = 0x0A,
    NRF_UPSTREAM_MESSAGE_SENSOR_SCAN = 0x0F,
    NRF_UPSTREAM_MESSAGE_STATUS = 0x80,
} nrf_upstream_message_type_t;

typedef enum {
    NRF_UPSTREAM_ROLE_UNASSIGNED = 0,
    NRF_UPSTREAM_ROLE_MAIN = 1,
    NRF_UPSTREAM_ROLE_REMOTE = 2,
} nrf_upstream_role_t;

typedef enum {
    NRF_UPSTREAM_LIFECYCLE_BOOTING = 0,
    NRF_UPSTREAM_LIFECYCLE_READY = 1,
    NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE = 2,
} nrf_upstream_lifecycle_t;

typedef enum {
    NRF_UPSTREAM_RESULT_OK = 0,
    NRF_UPSTREAM_RESULT_PENDING = 1,
    NRF_UPSTREAM_RESULT_INVALID_FRAME = 2,
    NRF_UPSTREAM_RESULT_BAD_CRC = 3,
    NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION = 4,
    NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE = 5,
    NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID = 6,
    NRF_UPSTREAM_RESULT_HW_ID_MISMATCH = 7,
    NRF_UPSTREAM_RESULT_ROLE_INVALID = 8,
    NRF_UPSTREAM_RESULT_NODE_ID_INVALID = 9,
    NRF_UPSTREAM_RESULT_CHANNEL_INVALID = 10,
    NRF_UPSTREAM_RESULT_ROLE_CONFLICT = 11,
    NRF_UPSTREAM_RESULT_REQUEST_CONFLICT = 12,
    NRF_UPSTREAM_RESULT_BUSY = 13,
    NRF_UPSTREAM_RESULT_READDRESS_FAILED = 14,
    NRF_UPSTREAM_RESULT_INTERNAL_ERROR = 15,
} nrf_upstream_result_t;

typedef enum {
    NRF_UPSTREAM_SENSOR_RESULT_OK = 0,
    NRF_UPSTREAM_SENSOR_RESULT_PENDING = 1,
    NRF_UPSTREAM_SENSOR_RESULT_PARTIAL = 2,
    NRF_UPSTREAM_SENSOR_RESULT_NOT_READY = 3,
    NRF_UPSTREAM_SENSOR_RESULT_NOT_REMOTE = 4,
    NRF_UPSTREAM_SENSOR_RESULT_NO_DATA = 5,
    NRF_UPSTREAM_SENSOR_RESULT_BUSY = 6,
    NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR = 7,
} nrf_upstream_sensor_result_t;

typedef struct {
    uint32_t request_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t role;
    uint8_t node_id;
    uint8_t mux_channel;
} nrf_upstream_assign_t;

typedef struct {
    uint32_t request_id;
    uint8_t hardware_id_length;
    uint8_t hardware_id[NRF_UPSTREAM_PROTOCOL_HW_ID_MAX];
    uint8_t role;
    uint8_t node_id;
    uint8_t mux_channel;
    uint8_t target_address;
    uint8_t lifecycle;
    uint8_t result;
    uint8_t flags;
} nrf_upstream_status_t;

typedef struct {
    uint32_t request_id;
} nrf_upstream_sensor_refresh_t;

typedef struct {
    uint8_t action;
    uint32_t request_id;
    uint16_t generation;
} nrf_upstream_height_reference_t;

typedef struct {
    uint8_t action;
    uint32_t request_id;
    uint16_t generation;
} nrf_upstream_imu_calibrate_t;

typedef struct {
    uint16_t offset;
} nrf_upstream_read_window_t;

typedef struct {
    uint32_t request_id;
    uint32_t sequence;
    uint32_t valid_mask;
    uint32_t fault_mask;
    uint64_t sample_complete_uptime_ms;
    uint8_t result;
    uint8_t flags;
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t imu_temperature_milli_c;
    int32_t env_temperature_milli_c;
    uint32_t pressure_pa;
    uint32_t humidity_milli_percent;
    /*
     * Processed on the nRF, sent so ESP32 does not need to redo this work.
     * altitude_mm validity follows the ENV valid_mask bit above (it is a
     * pure function of pressure_pa). orientation_state/blink_class are
     * self-describing: NRF_ORIENTATION_STATE_UNAVAILABLE already means
     * "don't trust this," so neither needs its own valid_mask bit.
     */
    int32_t altitude_mm;
    uint8_t orientation_state;
    uint8_t orientation_blink_class;
    uint32_t orientation_tilt_sin2_ppm;
    int32_t relative_z_mm;
    uint8_t reference_state;
    uint16_t reference_generation;
    int16_t gyro_bias_mdps[3];
    int16_t gravity_ref_mg[3];
    uint8_t imu_cal_state;
    uint16_t imu_cal_generation;
} nrf_upstream_sensor_snapshot_t;

uint16_t nrf_upstream_protocol_crc16_ccitt_false(const uint8_t *data, size_t length);
uint32_t nrf_upstream_protocol_request_id_hint(const uint8_t *frame, size_t length);
uint8_t nrf_upstream_protocol_address_for_role(uint8_t role);
nrf_upstream_result_t nrf_upstream_protocol_validate_assign(const nrf_upstream_assign_t *assign);

nrf_upstream_result_t nrf_upstream_protocol_encode_assign(
    const nrf_upstream_assign_t *assign,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_assign(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_assign_t *assign);

nrf_upstream_result_t nrf_upstream_protocol_encode_status(
    const nrf_upstream_status_t *status,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_status(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_status_t *status);

nrf_upstream_result_t nrf_upstream_protocol_encode_sensor_refresh(
    const nrf_upstream_sensor_refresh_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_sensor_refresh(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_sensor_refresh_t *request);

nrf_upstream_result_t nrf_upstream_protocol_encode_sensor_scan(
    const nrf_upstream_sensor_refresh_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_SCAN_LENGTH]);
nrf_upstream_result_t nrf_upstream_protocol_decode_sensor_scan(
    const uint8_t *frame, size_t length, nrf_upstream_sensor_refresh_t *request);

nrf_upstream_result_t nrf_upstream_protocol_encode_height_reference(
    const nrf_upstream_height_reference_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_height_reference(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_height_reference_t *request);

nrf_upstream_result_t nrf_upstream_protocol_encode_imu_calibrate(
    const nrf_upstream_imu_calibrate_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_IMU_CALIBRATE_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_imu_calibrate(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_imu_calibrate_t *request);

/*
 * Frame: common header (6), offset le16, two reserved zero bytes, CRC16.
 * The offset selects where in the last response the next read starts; it must
 * lie inside the largest response (the 136 byte snapshot / log block).
 */
nrf_upstream_result_t nrf_upstream_protocol_encode_read_window(
    const nrf_upstream_read_window_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_READ_WINDOW_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_read_window(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_read_window_t *request);

nrf_upstream_result_t nrf_upstream_protocol_encode_remote_snapshot(
    const nrf_upstream_status_t *status,
    const nrf_upstream_sensor_snapshot_t *sensor,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH]);

nrf_upstream_result_t nrf_upstream_protocol_decode_remote_snapshot(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_status_t *status,
    nrf_upstream_sensor_snapshot_t *sensor);

#ifdef __cplusplus
}
#endif

#endif
