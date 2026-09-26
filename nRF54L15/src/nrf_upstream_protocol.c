#include "nrf_upstream_protocol.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define NRF_UPSTREAM_MAGIC_0 0x47U
#define NRF_UPSTREAM_MAGIC_1 0x53U
#define NRF_UPSTREAM_HEADER_LENGTH 10U
#define NRF_UPSTREAM_SENSOR_MAGIC_0 0x53U
#define NRF_UPSTREAM_SENSOR_MAGIC_1 0x4EU
#define NRF_UPSTREAM_SENSOR_EXTENSION_VERSION 2U
#define NRF_UPSTREAM_SENSOR_EXTENSION_VERSION_V1 1U
#define NRF_UPSTREAM_SENSOR_EXTENSION_TYPE_SNAPSHOT 1U

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_le64(const uint8_t *data)
{
    return (uint64_t)read_le32(data) |
           ((uint64_t)read_le32(&data[4]) << 32U);
}

static int32_t read_le_i32(const uint8_t *data)
{
    const uint32_t raw = read_le32(data);
    if (raw <= (uint32_t)INT32_MAX) {
        return (int32_t)raw;
    }
    return -1 - (int32_t)(UINT32_MAX - raw);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8U) & 0xFFU);
    data[2] = (uint8_t)((value >> 16U) & 0xFFU);
    data[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void write_le64(uint8_t *data, uint64_t value)
{
    write_le32(data, (uint32_t)(value & UINT64_C(0xFFFFFFFF)));
    write_le32(&data[4], (uint32_t)(value >> 32U));
}

static void write_le_i32(uint8_t *data, int32_t value)
{
    uint32_t raw;
    if (value >= 0) {
        raw = (uint32_t)value;
    } else {
        raw = UINT32_MAX - (uint32_t)(-(int64_t)value - 1);
    }
    write_le32(data, raw);
}

static void write_common_header(uint8_t *frame, uint8_t type, uint16_t length)
{
    frame[0] = NRF_UPSTREAM_MAGIC_0;
    frame[1] = NRF_UPSTREAM_MAGIC_1;
    frame[2] = NRF_UPSTREAM_PROTOCOL_VERSION;
    frame[3] = type;
    write_le16(&frame[4], length);
}

static void write_header(uint8_t *frame, uint8_t type, uint16_t length, uint32_t request_id)
{
    write_common_header(frame, type, length);
    write_le32(&frame[6], request_id);
}

static nrf_upstream_result_t validate_hardware_id(uint8_t length, const uint8_t hardware_id[8])
{
    size_t index;

    if (length == 0U || length > NRF_UPSTREAM_PROTOCOL_HW_ID_MAX) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    for (index = length; index < NRF_UPSTREAM_PROTOCOL_HW_ID_MAX; ++index) {
        if (hardware_id[index] != 0U) {
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
    }
    return NRF_UPSTREAM_RESULT_OK;
}

static nrf_upstream_result_t validate_sensor_snapshot(
    const nrf_upstream_sensor_snapshot_t *sensor)
{
    const uint32_t invalid_mask =
        (sensor->valid_mask | sensor->fault_mask) & ~NRF_UPSTREAM_SENSOR_VALID_MASK;
    const bool has_sample = sensor->valid_mask != 0U;
    const bool sample_flag =
        (sensor->flags & NRF_UPSTREAM_SENSOR_FLAG_SAMPLE_AVAILABLE) != 0U;

    if (sensor->result > NRF_UPSTREAM_SENSOR_RESULT_INTERNAL_ERROR ||
        (sensor->flags & ~NRF_UPSTREAM_SENSOR_FLAG_MASK) != 0U ||
        invalid_mask != 0U ||
        (sensor->valid_mask & sensor->fault_mask) != 0U ||
        has_sample != sample_flag ||
        sensor->orientation_state > NRF_UPSTREAM_ORIENTATION_STATE_MAX ||
        sensor->orientation_blink_class > NRF_UPSTREAM_ORIENTATION_BLINK_MAX ||
        sensor->reference_state > NRF_UPSTREAM_REFERENCE_SENSOR_INVALID ||
        (sensor->reference_state != NRF_UPSTREAM_REFERENCE_VALID &&
         sensor->relative_z_mm != 0)) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_validate_assign(const nrf_upstream_assign_t *assign)
{
    nrf_upstream_result_t result;

    if (assign == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    result = validate_hardware_id(assign->hardware_id_length, assign->hardware_id);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }
    if (assign->request_id == 0U) {
        return NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID;
    }
    if (assign->role != NRF_UPSTREAM_ROLE_MAIN && assign->role != NRF_UPSTREAM_ROLE_REMOTE) {
        return NRF_UPSTREAM_RESULT_ROLE_INVALID;
    }
    if (assign->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED) {
        return NRF_UPSTREAM_RESULT_NODE_ID_INVALID;
    }
    if (assign->role == NRF_UPSTREAM_ROLE_MAIN) {
        if (assign->mux_channel != NRF_UPSTREAM_MUX_CHANNEL_NONE) {
            return NRF_UPSTREAM_RESULT_CHANNEL_INVALID;
        }
    }
    else if (assign->mux_channel > 7U) {
        return NRF_UPSTREAM_RESULT_CHANNEL_INVALID;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

uint16_t nrf_upstream_protocol_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    size_t index;
    unsigned int bit;

    if (data == NULL && length != 0U) {
        return 0U;
    }

    for (index = 0U; index < length; ++index) {
        crc ^= (uint16_t)data[index] << 8U;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                ? (uint16_t)((crc << 1U) ^ 0x1021U)
                : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

uint32_t nrf_upstream_protocol_request_id_hint(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < NRF_UPSTREAM_HEADER_LENGTH ||
        frame[0] != NRF_UPSTREAM_MAGIC_0 || frame[1] != NRF_UPSTREAM_MAGIC_1) {
        return 0U;
    }
    return read_le32(&frame[6]);
}

uint8_t nrf_upstream_protocol_address_for_role(uint8_t role)
{
    if (role == NRF_UPSTREAM_ROLE_MAIN) {
        return NRF_UPSTREAM_ADDRESS_MAIN;
    }
    if (role == NRF_UPSTREAM_ROLE_REMOTE) {
        return NRF_UPSTREAM_ADDRESS_REMOTE;
    }
    return 0U;
}

nrf_upstream_result_t nrf_upstream_protocol_encode_assign(
    const nrf_upstream_assign_t *assign,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH])
{
    nrf_upstream_result_t result;
    uint16_t crc;

    if (assign == NULL || frame == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    result = nrf_upstream_protocol_validate_assign(assign);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }

    memset(frame, 0, NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH);
    write_header(
        frame,
        NRF_UPSTREAM_MESSAGE_ASSIGN,
        NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH,
        assign->request_id);
    frame[10] = assign->hardware_id_length;
    memcpy(&frame[11], assign->hardware_id, assign->hardware_id_length);
    frame[19] = assign->role;
    frame[20] = assign->node_id;
    frame[21] = assign->mux_channel;
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 22U);
    write_le16(&frame[22], crc);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_decode_assign(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_assign_t *assign)
{
    uint16_t expected_crc;
    uint16_t actual_crc;
    nrf_upstream_result_t result;

    if (frame == NULL || assign == NULL || length != NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (frame[0] != NRF_UPSTREAM_MAGIC_0 || frame[1] != NRF_UPSTREAM_MAGIC_1) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (read_le16(&frame[4]) != NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    expected_crc = read_le16(&frame[22]);
    actual_crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 22U);
    if (actual_crc != expected_crc) {
        return NRF_UPSTREAM_RESULT_BAD_CRC;
    }
    if (frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION;
    }
    if (frame[3] != NRF_UPSTREAM_MESSAGE_ASSIGN) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE;
    }

    memset(assign, 0, sizeof(*assign));
    assign->request_id = read_le32(&frame[6]);
    assign->hardware_id_length = frame[10];
    memcpy(assign->hardware_id, &frame[11], NRF_UPSTREAM_PROTOCOL_HW_ID_MAX);
    assign->role = frame[19];
    assign->node_id = frame[20];
    assign->mux_channel = frame[21];

    result = nrf_upstream_protocol_validate_assign(assign);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        memset(assign, 0, sizeof(*assign));
    }
    return result;
}

nrf_upstream_result_t nrf_upstream_protocol_encode_status(
    const nrf_upstream_status_t *status,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH])
{
    nrf_upstream_result_t result;
    uint16_t crc;

    if (status == NULL || frame == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    result = validate_hardware_id(status->hardware_id_length, status->hardware_id);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }
    if (status->role > NRF_UPSTREAM_ROLE_REMOTE ||
        status->lifecycle > NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE ||
        status->result > NRF_UPSTREAM_RESULT_INTERNAL_ERROR ||
        status->target_address > 0x7FU ||
        (status->flags & ~(NRF_UPSTREAM_STATUS_FLAG_ASSIGNED |
                           NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED)) != 0U) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (status->role == NRF_UPSTREAM_ROLE_UNASSIGNED) {
        if (status->node_id != NRF_UPSTREAM_NODE_ID_UNASSIGNED ||
            status->mux_channel != NRF_UPSTREAM_MUX_CHANNEL_NONE) {
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
    }
    else if (status->role == NRF_UPSTREAM_ROLE_MAIN) {
        if (status->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED ||
            status->mux_channel != NRF_UPSTREAM_MUX_CHANNEL_NONE) {
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
    }
    else if (status->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED || status->mux_channel > 7U) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    memset(frame, 0, NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH);
    write_header(
        frame,
        NRF_UPSTREAM_MESSAGE_STATUS,
        NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH,
        status->request_id);
    frame[10] = status->hardware_id_length;
    memcpy(&frame[11], status->hardware_id, status->hardware_id_length);
    frame[19] = status->role;
    frame[20] = status->node_id;
    frame[21] = status->mux_channel;
    frame[22] = status->target_address;
    frame[23] = status->lifecycle;
    frame[24] = status->result;
    frame[25] = status->flags;
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 26U);
    write_le16(&frame[26], crc);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_decode_status(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_status_t *status)
{
    uint16_t expected_crc;
    uint16_t actual_crc;
    nrf_upstream_result_t result;

    if (frame == NULL || status == NULL || length != NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (frame[0] != NRF_UPSTREAM_MAGIC_0 || frame[1] != NRF_UPSTREAM_MAGIC_1 ||
        read_le16(&frame[4]) != NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    expected_crc = read_le16(&frame[26]);
    actual_crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 26U);
    if (actual_crc != expected_crc) {
        return NRF_UPSTREAM_RESULT_BAD_CRC;
    }
    if (frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION;
    }
    if (frame[3] != NRF_UPSTREAM_MESSAGE_STATUS) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE;
    }

    memset(status, 0, sizeof(*status));
    status->request_id = read_le32(&frame[6]);
    status->hardware_id_length = frame[10];
    memcpy(status->hardware_id, &frame[11], NRF_UPSTREAM_PROTOCOL_HW_ID_MAX);
    status->role = frame[19];
    status->node_id = frame[20];
    status->mux_channel = frame[21];
    status->target_address = frame[22];
    status->lifecycle = frame[23];
    status->result = frame[24];
    status->flags = frame[25];

    result = validate_hardware_id(status->hardware_id_length, status->hardware_id);
    if (result != NRF_UPSTREAM_RESULT_OK ||
        status->role > NRF_UPSTREAM_ROLE_REMOTE ||
        status->lifecycle > NRF_UPSTREAM_LIFECYCLE_UNAVAILABLE ||
        status->result > NRF_UPSTREAM_RESULT_INTERNAL_ERROR ||
        status->target_address > 0x7FU ||
        (status->flags & ~(NRF_UPSTREAM_STATUS_FLAG_ASSIGNED |
                           NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED)) != 0U) {
        memset(status, 0, sizeof(*status));
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (status->role == NRF_UPSTREAM_ROLE_UNASSIGNED) {
        if (status->node_id != NRF_UPSTREAM_NODE_ID_UNASSIGNED ||
            status->mux_channel != NRF_UPSTREAM_MUX_CHANNEL_NONE) {
            memset(status, 0, sizeof(*status));
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
    }
    else if (status->role == NRF_UPSTREAM_ROLE_MAIN) {
        if (status->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED ||
            status->mux_channel != NRF_UPSTREAM_MUX_CHANNEL_NONE) {
            memset(status, 0, sizeof(*status));
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
    }
    else if (status->node_id == NRF_UPSTREAM_NODE_ID_UNASSIGNED || status->mux_channel > 7U) {
        memset(status, 0, sizeof(*status));
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_encode_sensor_refresh(
    const nrf_upstream_sensor_refresh_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH])
{
    uint16_t crc;

    if (request == NULL || frame == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (request->request_id == 0U) {
        return NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID;
    }

    memset(frame, 0, NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH);
    write_header(
        frame,
        NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH,
        NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH,
        request->request_id);
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 10U);
    write_le16(&frame[10], crc);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_decode_sensor_refresh(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_sensor_refresh_t *request)
{
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (frame == NULL || request == NULL ||
        length != NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (frame[0] != NRF_UPSTREAM_MAGIC_0 || frame[1] != NRF_UPSTREAM_MAGIC_1 ||
        read_le16(&frame[4]) != NRF_UPSTREAM_PROTOCOL_SENSOR_REFRESH_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    expected_crc = read_le16(&frame[10]);
    actual_crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 10U);
    if (actual_crc != expected_crc) {
        return NRF_UPSTREAM_RESULT_BAD_CRC;
    }
    if (frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION;
    }
    if (frame[3] != NRF_UPSTREAM_MESSAGE_SENSOR_REFRESH) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE;
    }

    request->request_id = read_le32(&frame[6]);
    if (request->request_id == 0U) {
        request->request_id = 0U;
        return NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_encode_height_reference(
    const nrf_upstream_height_reference_t *request,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH])
{
    uint16_t crc;

    if (request == NULL || frame == NULL ||
        (request->action != NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE &&
         request->action != NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR)) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (request->request_id == 0U) {
        return NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID;
    }

    memset(frame, 0, NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH);
    write_common_header(
        frame,
        NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE,
        NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH);
    frame[6] = request->action;
    frame[7] = 0U;
    write_le32(&frame[8], request->request_id);
    write_le16(&frame[12], request->generation);
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 14U);
    write_le16(&frame[14], crc);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_decode_height_reference(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_height_reference_t *request)
{
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (frame == NULL || request == NULL ||
        length != NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH ||
        frame[0] != NRF_UPSTREAM_MAGIC_0 || frame[1] != NRF_UPSTREAM_MAGIC_1 ||
        read_le16(&frame[4]) != NRF_UPSTREAM_PROTOCOL_HEIGHT_REFERENCE_LENGTH) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    expected_crc = read_le16(&frame[14]);
    actual_crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 14U);
    if (actual_crc != expected_crc) {
        return NRF_UPSTREAM_RESULT_BAD_CRC;
    }
    if (frame[2] != NRF_UPSTREAM_PROTOCOL_VERSION) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION;
    }
    if (frame[3] != NRF_UPSTREAM_MESSAGE_HEIGHT_REFERENCE) {
        return NRF_UPSTREAM_RESULT_UNSUPPORTED_TYPE;
    }
    if (frame[7] != 0U ||
        (frame[6] != NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE &&
         frame[6] != NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR)) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    request->action = frame[6];
    request->request_id = read_le32(&frame[8]);
    request->generation = read_le16(&frame[12]);
    if (request->request_id == 0U) {
        memset(request, 0, sizeof(*request));
        return NRF_UPSTREAM_RESULT_REQUEST_ID_INVALID;
    }
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_encode_remote_snapshot(
    const nrf_upstream_status_t *status,
    const nrf_upstream_sensor_snapshot_t *sensor,
    uint8_t frame[NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH])
{
    nrf_upstream_result_t result;
    uint16_t crc;
    size_t index;

    if (status == NULL || sensor == NULL || frame == NULL) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    if (status->role != NRF_UPSTREAM_ROLE_REMOTE ||
        status->target_address != NRF_UPSTREAM_ADDRESS_REMOTE) {
        return NRF_UPSTREAM_RESULT_ROLE_INVALID;
    }
    result = validate_sensor_snapshot(sensor);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }

    memset(frame, 0, NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    result = nrf_upstream_protocol_encode_status(status, frame);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }

    frame[28] = NRF_UPSTREAM_SENSOR_MAGIC_0;
    frame[29] = NRF_UPSTREAM_SENSOR_MAGIC_1;
    frame[30] = NRF_UPSTREAM_SENSOR_EXTENSION_VERSION;
    frame[31] = NRF_UPSTREAM_SENSOR_EXTENSION_TYPE_SNAPSHOT;
    write_le16(&frame[32], NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH);
    write_le32(&frame[34], sensor->request_id);
    write_le32(&frame[38], sensor->sequence);
    write_le32(&frame[42], sensor->valid_mask);
    write_le32(&frame[46], sensor->fault_mask);
    write_le64(&frame[50], sensor->sample_complete_uptime_ms);
    frame[58] = sensor->result;
    frame[59] = sensor->flags;
    for (index = 0U; index < 3U; ++index) {
        write_le_i32(&frame[60U + (index * 4U)], sensor->accel_mg[index]);
        write_le_i32(&frame[72U + (index * 4U)], sensor->gyro_mdps[index]);
    }
    write_le_i32(&frame[84], sensor->imu_temperature_milli_c);
    write_le_i32(&frame[88], sensor->env_temperature_milli_c);
    write_le32(&frame[92], sensor->pressure_pa);
    write_le32(&frame[96], sensor->humidity_milli_percent);
    write_le_i32(&frame[100], sensor->altitude_mm);
    frame[104] = sensor->orientation_state;
    frame[105] = sensor->orientation_blink_class;
    write_le32(&frame[106], sensor->orientation_tilt_sin2_ppm);
    write_le_i32(&frame[110], sensor->relative_z_mm);
    frame[114] = sensor->reference_state;
    frame[115] = 0U;
    write_le16(&frame[116], sensor->reference_generation);
    crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 118U);
    write_le16(&frame[118], crc);
    return NRF_UPSTREAM_RESULT_OK;
}

nrf_upstream_result_t nrf_upstream_protocol_decode_remote_snapshot(
    const uint8_t *frame,
    size_t length,
    nrf_upstream_status_t *status,
    nrf_upstream_sensor_snapshot_t *sensor)
{
    nrf_upstream_result_t result;
    uint16_t expected_crc;
    uint16_t actual_crc;
    size_t crc_offset;
    uint8_t extension_version;
    uint16_t extension_length;
    size_t index;

    if (frame == NULL || status == NULL || sensor == NULL ||
        (length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH &&
         length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH)) {
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }
    result = nrf_upstream_protocol_decode_status(
        frame,
        NRF_UPSTREAM_PROTOCOL_STATUS_LENGTH,
        status);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        return result;
    }
    extension_version = frame[30];
    extension_length = read_le16(&frame[32]);
    if (status->role != NRF_UPSTREAM_ROLE_REMOTE ||
        status->target_address != NRF_UPSTREAM_ADDRESS_REMOTE ||
        frame[28] != NRF_UPSTREAM_SENSOR_MAGIC_0 ||
        frame[29] != NRF_UPSTREAM_SENSOR_MAGIC_1 ||
        frame[31] != NRF_UPSTREAM_SENSOR_EXTENSION_TYPE_SNAPSHOT ||
        (extension_version == NRF_UPSTREAM_SENSOR_EXTENSION_VERSION_V1 &&
         (length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH ||
          extension_length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_V1_LENGTH)) ||
        (extension_version == NRF_UPSTREAM_SENSOR_EXTENSION_VERSION &&
         (length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH ||
          extension_length != NRF_UPSTREAM_PROTOCOL_REMOTE_SNAPSHOT_LENGTH)) ||
        (extension_version != NRF_UPSTREAM_SENSOR_EXTENSION_VERSION_V1 &&
         extension_version != NRF_UPSTREAM_SENSOR_EXTENSION_VERSION)) {
        memset(sensor, 0, sizeof(*sensor));
        return NRF_UPSTREAM_RESULT_INVALID_FRAME;
    }

    crc_offset = extension_version == NRF_UPSTREAM_SENSOR_EXTENSION_VERSION_V1
        ? 110U
        : 118U;
    expected_crc = read_le16(&frame[crc_offset]);
    actual_crc = nrf_upstream_protocol_crc16_ccitt_false(frame, crc_offset);
    if (actual_crc != expected_crc) {
        memset(sensor, 0, sizeof(*sensor));
        return NRF_UPSTREAM_RESULT_BAD_CRC;
    }

    memset(sensor, 0, sizeof(*sensor));
    sensor->request_id = read_le32(&frame[34]);
    sensor->sequence = read_le32(&frame[38]);
    sensor->valid_mask = read_le32(&frame[42]);
    sensor->fault_mask = read_le32(&frame[46]);
    sensor->sample_complete_uptime_ms = read_le64(&frame[50]);
    sensor->result = frame[58];
    sensor->flags = frame[59];
    for (index = 0U; index < 3U; ++index) {
        sensor->accel_mg[index] = read_le_i32(&frame[60U + (index * 4U)]);
        sensor->gyro_mdps[index] = read_le_i32(&frame[72U + (index * 4U)]);
    }
    sensor->imu_temperature_milli_c = read_le_i32(&frame[84]);
    sensor->env_temperature_milli_c = read_le_i32(&frame[88]);
    sensor->pressure_pa = read_le32(&frame[92]);
    sensor->humidity_milli_percent = read_le32(&frame[96]);
    sensor->altitude_mm = read_le_i32(&frame[100]);
    sensor->orientation_state = frame[104];
    sensor->orientation_blink_class = frame[105];
    sensor->orientation_tilt_sin2_ppm = read_le32(&frame[106]);
    if (extension_version == NRF_UPSTREAM_SENSOR_EXTENSION_VERSION) {
        if (frame[115] != 0U) {
            memset(sensor, 0, sizeof(*sensor));
            return NRF_UPSTREAM_RESULT_INVALID_FRAME;
        }
        sensor->relative_z_mm = read_le_i32(&frame[110]);
        sensor->reference_state = frame[114];
        sensor->reference_generation = read_le16(&frame[116]);
    } else {
        sensor->relative_z_mm = 0;
        sensor->reference_state = NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED;
        sensor->reference_generation = 0U;
    }

    result = validate_sensor_snapshot(sensor);
    if (result != NRF_UPSTREAM_RESULT_OK) {
        memset(sensor, 0, sizeof(*sensor));
    }
    return result;
}
