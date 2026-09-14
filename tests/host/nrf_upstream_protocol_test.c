#include <assert.h>
#include <stdint.h>
#include <string.h>

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

static void refresh_crc(uint8_t frame[NRF_UPSTREAM_PROTOCOL_ASSIGN_LENGTH])
{
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, 22U);
    frame[22] = (uint8_t)(crc & 0xFFU);
    frame[23] = (uint8_t)(crc >> 8U);
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
    refresh_crc(frame);
    assert(nrf_upstream_protocol_decode_assign(frame, sizeof(frame), &decoded) == NRF_UPSTREAM_RESULT_UNSUPPORTED_VERSION);

    assert(nrf_upstream_protocol_encode_assign(&assign, frame) == NRF_UPSTREAM_RESULT_OK);
    frame[3] = 0x44U;
    refresh_crc(frame);
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
    test_role_address_contract();
    return 0;
}
