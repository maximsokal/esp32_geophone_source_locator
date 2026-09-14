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

#define NRF_UPSTREAM_ADDRESS_DISCOVERY 0x30U
#define NRF_UPSTREAM_ADDRESS_MAIN      0x31U
#define NRF_UPSTREAM_ADDRESS_REMOTE    0x32U

#define NRF_UPSTREAM_NODE_ID_UNASSIGNED 0xFFU
#define NRF_UPSTREAM_MUX_CHANNEL_NONE   0xFFU

#define NRF_UPSTREAM_STATUS_FLAG_ASSIGNED          0x01U
#define NRF_UPSTREAM_STATUS_FLAG_TARGET_REGISTERED 0x02U

typedef enum {
    NRF_UPSTREAM_MESSAGE_ASSIGN = 0x01,
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

#ifdef __cplusplus
}
#endif

#endif
