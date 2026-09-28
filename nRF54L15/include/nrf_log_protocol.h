#ifndef NRF_LOG_PROTOCOL_H
#define NRF_LOG_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRF_LOG_PROTOCOL_VERSION 1U
#define NRF_LOG_PROTOCOL_HEADER_LENGTH 10U
#define NRF_LOG_PROTOCOL_CRC_LENGTH 2U
#define NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH 12U
#define NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH 22U
#define NRF_LOG_PROTOCOL_STATUS_LENGTH 38U
#define NRF_LOG_PROTOCOL_RECORD_LENGTH 20U
#define NRF_LOG_PROTOCOL_MAX_RECORDS 5U
#define NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH 36U
#define NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH 136U

typedef enum {
    NRF_LOG_MESSAGE_STATUS_REQUEST = 0x07,
    NRF_LOG_MESSAGE_READ_REQUEST = 0x08,
    NRF_LOG_MESSAGE_STATUS = 0x82,
    NRF_LOG_MESSAGE_BLOCK = 0x83,
} nrf_log_message_type_t;

typedef enum {
    NRF_LOG_SEVERITY_DEBUG = 0,
    NRF_LOG_SEVERITY_INFO = 1,
    NRF_LOG_SEVERITY_WARN = 2,
    NRF_LOG_SEVERITY_ERROR = 3,
    NRF_LOG_SEVERITY_FATAL = 4,
} nrf_log_severity_t;

typedef enum {
    NRF_LOG_SUBSYSTEM_INVALID = 0,
    NRF_LOG_SUBSYSTEM_BOOT = 1,
    NRF_LOG_SUBSYSTEM_LOG = 2,
    NRF_LOG_SUBSYSTEM_TWIS = 3,
    NRF_LOG_SUBSYSTEM_TOPOLOGY = 4,
    NRF_LOG_SUBSYSTEM_SENSOR = 5,
    NRF_LOG_SUBSYSTEM_CALIBRATION = 6,
    NRF_LOG_SUBSYSTEM_CS = 7,
    NRF_LOG_SUBSYSTEM_WATCHDOG = 8,
    NRF_LOG_SUBSYSTEM_POWER = 9,
} nrf_log_subsystem_t;

typedef enum {
    NRF_LOG_ERROR_OK = 0,
    NRF_LOG_ERROR_NULL_ARGUMENT,
    NRF_LOG_ERROR_BAD_MAGIC,
    NRF_LOG_ERROR_BAD_VERSION,
    NRF_LOG_ERROR_BAD_TYPE,
    NRF_LOG_ERROR_BAD_LENGTH,
    NRF_LOG_ERROR_BAD_CRC,
    NRF_LOG_ERROR_RESERVED_NONZERO,
    NRF_LOG_ERROR_BAD_FLAGS,
    NRF_LOG_ERROR_BAD_SEVERITY,
    NRF_LOG_ERROR_BAD_SUBSYSTEM,
    NRF_LOG_ERROR_COUNT_OVERFLOW,
    NRF_LOG_ERROR_SEQ_MISMATCH,
    NRF_LOG_ERROR_UPTIME_REGRESSION,
    NRF_LOG_ERROR_BAD_BOOT_ID,
    NRF_LOG_ERROR_INVARIANT_VIOLATION,
    NRF_LOG_ERROR_REENTRANT_LOSS,
} nrf_log_error_t;

enum {
    NRF_LOG_STATUS_FLAG_ENABLED = 0x01U,
    NRF_LOG_STATUS_FLAG_BOOT_ID_FALLBACK = 0x02U,
    NRF_LOG_STATUS_FLAG_MASK = 0x03U,
    NRF_LOG_BLOCK_FLAG_MORE = 0x01U,
    NRF_LOG_BLOCK_FLAG_GAP = 0x02U,
    NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH = 0x04U,
    NRF_LOG_BLOCK_FLAG_MASK = 0x07U,
};

typedef struct {
    uint16_t id;
    const char *name;
    uint8_t subsystem;
    uint8_t default_severity;
} nrf_log_event_definition_t;

extern const nrf_log_event_definition_t nrf_log_event_definitions[];
extern const size_t nrf_log_event_definition_count;
const nrf_log_event_definition_t *nrf_log_event_find(uint16_t id);

typedef struct {
    uint32_t request_id;
} nrf_log_status_request_t;

typedef struct {
    uint32_t request_id;
    uint32_t expected_boot_id;
    uint32_t cursor_seq;
    uint8_t max_records;
} nrf_log_read_request_t;

typedef struct {
    uint32_t request_id;
    uint8_t flags;
    uint16_t capacity;
    uint16_t available;
    uint32_t boot_id;
    uint32_t oldest_seq;
    uint32_t next_seq;
    uint32_t overwritten_total;
    uint32_t lost_internal_total;
} nrf_log_status_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    uint16_t event_id;
    uint8_t severity;
    uint8_t subsystem;
    uint32_t arg0;
    uint32_t arg1;
} nrf_log_record_t;

typedef struct {
    uint32_t request_id;
    uint8_t flags;
    uint8_t record_count;
    uint32_t boot_id;
    uint32_t first_seq;
    uint32_t next_cursor;
    uint32_t oldest_seq;
    uint32_t overwritten_total;
    nrf_log_record_t records[NRF_LOG_PROTOCOL_MAX_RECORDS];
} nrf_log_block_t;

typedef struct {
    bool has_expected_boot_id;
    uint32_t expected_boot_id;
    bool has_cursor;
    uint32_t cursor_seq;
    bool has_max_records;
    uint8_t max_records;
    bool has_status_next_seq;
    uint32_t status_next_seq;
} nrf_log_block_validation_t;

nrf_log_error_t nrf_log_protocol_encode_record(
    const nrf_log_record_t *record,
    uint8_t bytes[NRF_LOG_PROTOCOL_RECORD_LENGTH]);
nrf_log_error_t nrf_log_protocol_decode_record(
    const uint8_t bytes[NRF_LOG_PROTOCOL_RECORD_LENGTH],
    nrf_log_record_t *record);

nrf_log_error_t nrf_log_protocol_encode_status_request(
    const nrf_log_status_request_t *request,
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH]);
nrf_log_error_t nrf_log_protocol_decode_status_request(
    const uint8_t *frame,
    size_t length,
    nrf_log_status_request_t *request);
nrf_log_error_t nrf_log_protocol_encode_read_request(
    const nrf_log_read_request_t *request,
    uint8_t frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH]);
nrf_log_error_t nrf_log_protocol_decode_read_request(
    const uint8_t *frame,
    size_t length,
    nrf_log_read_request_t *request);
nrf_log_error_t nrf_log_protocol_encode_status(
    const nrf_log_status_t *status,
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH]);
nrf_log_error_t nrf_log_protocol_decode_status(
    const uint8_t *frame,
    size_t length,
    nrf_log_status_t *status);
nrf_log_error_t nrf_log_protocol_encode_block(
    const nrf_log_block_t *block,
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH],
    size_t *length);
nrf_log_error_t nrf_log_protocol_decode_block(
    const uint8_t *frame,
    size_t length,
    const nrf_log_block_validation_t *validation,
    nrf_log_block_t *block);

#ifdef __cplusplus
}
#endif

#endif
