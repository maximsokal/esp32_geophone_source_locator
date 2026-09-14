#include "nrf_upstream_protocol.h"

#include <string.h>

#define NRF_UPSTREAM_MAGIC_0 0x47U
#define NRF_UPSTREAM_MAGIC_1 0x53U
#define NRF_UPSTREAM_HEADER_LENGTH 10U

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

static void write_header(uint8_t *frame, uint8_t type, uint16_t length, uint32_t request_id)
{
    frame[0] = NRF_UPSTREAM_MAGIC_0;
    frame[1] = NRF_UPSTREAM_MAGIC_1;
    frame[2] = NRF_UPSTREAM_PROTOCOL_VERSION;
    frame[3] = type;
    write_le16(&frame[4], length);
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
