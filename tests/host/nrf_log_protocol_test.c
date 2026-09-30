#include "nrf_log_events.h"
#include "nrf_log_protocol.h"
#include "nrf_upstream_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t active_seed = 0x308C0FFEU;
static unsigned int checks;

typedef struct {
    uint8_t before[8];
    nrf_log_block_t block;
    uint8_t after[8];
} guarded_block_t;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "seed=0x%08lX failed at %s:%d: %s\n", \
            (unsigned long)active_seed, __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static const uint8_t GOLDEN_STATUS[NRF_LOG_PROTOCOL_STATUS_LENGTH] = {
    0x47, 0x53, 0x01, 0x82, 0x26, 0x00, 0x78, 0x56, 0x34, 0x12, 0x01, 0x03,
    0x80, 0x00, 0x02, 0x00, 0x44, 0x33, 0x22, 0x11, 0x05, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0xDD, 0xCC, 0xBB, 0xAA,
    0x81, 0x1A
};

/* Independent hand calculation: CRC over bytes 0..35 is 0x1A81; wire bytes are 81 1A. */
static const uint8_t GOLDEN_BLOCK[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH] = {
    0x47, 0x53, 0x01, 0x83, 0x88, 0x00, 0x21, 0x43, 0x65, 0x87, 0x01, 0x00,
    0x05, 0x00, 0xD4, 0xC3, 0xB2, 0xA1, 0x05, 0x00, 0x00, 0x00, 0x0A, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x04, 0x03,
    0x02, 0x01, 0x00, 0xC0, 0xB0, 0xA0, 0x06, 0x00, 0x00, 0x00, 0xC8, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x01, 0x02, 0x05, 0x03, 0x02, 0x01, 0x01, 0xC0,
    0xB0, 0xA0, 0x07, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x03, 0x00,
    0x02, 0x03, 0x06, 0x03, 0x02, 0x01, 0x02, 0xC0, 0xB0, 0xA0, 0x08, 0x00,
    0x00, 0x00, 0x90, 0x01, 0x00, 0x00, 0x04, 0x00, 0x03, 0x04, 0x07, 0x03,
    0x02, 0x01, 0x03, 0xC0, 0xB0, 0xA0, 0x09, 0x00, 0x00, 0x00, 0xF4, 0x01,
    0x00, 0x00, 0x05, 0x00, 0x04, 0x05, 0x08, 0x03, 0x02, 0x01, 0x04, 0xC0,
    0xB0, 0xA0, 0xB5, 0xC0
};

/* Independent hand calculation: CRC over bytes 0..133 is 0xC0B5; wire bytes are B5 C0. */

static void refresh_crc(uint8_t *frame, size_t length)
{
    uint16_t crc = nrf_upstream_protocol_crc16_ccitt_false(frame, length - 2U);
    frame[length - 2U] = (uint8_t)crc;
    frame[length - 1U] = (uint8_t)(crc >> 8U);
}

static void check_burst_corruption_rejected(const uint8_t *golden, size_t frame_length)
{
    static const uint8_t bursts[] = {2U, 4U, 8U, 16U, 32U};
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    nrf_log_status_t status;
    nrf_log_block_t block;

    for (size_t burst_index = 0U; burst_index < sizeof(bursts); ++burst_index) {
        const size_t burst_length = bursts[burst_index];
        const size_t total_bits = frame_length * 8U;
        for (size_t start = 0U; start <= total_bits - burst_length; ++start) {
            memcpy(frame, golden, frame_length);
            for (size_t bit = 0U; bit < burst_length; ++bit) {
                const size_t target = start + bit;
                frame[target / 8U] ^= (uint8_t)(1U << (target % 8U));
            }
            if (golden[3] == NRF_LOG_MESSAGE_STATUS) {
                CHECK(nrf_log_protocol_decode_status(frame, frame_length, &status) != NRF_LOG_ERROR_OK);
            } else {
                CHECK(nrf_log_protocol_decode_block(frame, frame_length, NULL, &block) != NRF_LOG_ERROR_OK);
            }
        }
    }
}

static void set_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void set_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void check_bytes(const uint8_t *bytes, size_t length, uint8_t expected)
{
    size_t i;
    for (i = 0U; i < length; ++i) {
        CHECK(bytes[i] == expected);
    }
}

static nrf_log_status_t golden_status(void)
{
    nrf_log_status_t status;
    status.request_id = 0x12345678U;
    status.flags = NRF_LOG_STATUS_FLAG_ENABLED | NRF_LOG_STATUS_FLAG_BOOT_ID_FALLBACK;
    status.capacity = 128U;
    status.available = 2U;
    status.boot_id = 0x11223344U;
    status.oldest_seq = 5U;
    status.next_seq = 7U;
    status.overwritten_total = 9U;
    status.lost_internal_total = 0xAABBCCDDU;
    return status;
}

static nrf_log_block_t golden_block(void)
{
    nrf_log_block_t block;
    uint8_t i;
    memset(&block, 0, sizeof(block));
    block.request_id = 0x87654321U;
    block.boot_id = 0xA1B2C3D4U;
    block.record_count = 5U;
    block.first_seq = 5U;
    block.next_cursor = 10U;
    block.oldest_seq = 0U;
    block.overwritten_total = 4U;
    for (i = 0U; i < block.record_count; ++i) {
        block.records[i].seq = 5U + i;
        block.records[i].uptime_ms = (uint32_t)(100U * (i + 1U));
        block.records[i].event_id = (uint16_t)(i + 1U);
        block.records[i].severity = i;
        block.records[i].subsystem = (uint8_t)(i + 1U);
        block.records[i].arg0 = 0x01020304U + i;
        block.records[i].arg1 = 0xA0B0C000U + i;
    }
    return block;
}

static void test_golden_vectors_and_roundtrip(void)
{
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t length = 0U;
    nrf_log_status_request_t status_request = {0x01020304U};
    nrf_log_status_request_t decoded_status_request;
    nrf_log_status_t status = golden_status();
    nrf_log_status_t decoded_status;
    nrf_log_block_t block = golden_block();
    nrf_log_block_t decoded_block;
    nrf_log_block_validation_t validation = {true, 0xA1B2C3D4U, true, 5U, true, 5U, true, 10U};
    static const uint8_t GOLDEN_STATUS_REQUEST[12] = {
        0x47, 0x53, 0x01, 0x07, 0x0C, 0x00, 0x04, 0x03, 0x02, 0x01, 0x2C, 0x1E
    };

    /* Independent hand calculation: CRC over bytes 0..9 is 0x1E2C; wire bytes are 2C 1E. */
    CHECK(nrf_log_protocol_encode_status_request(&status_request, frame) == NRF_LOG_ERROR_OK);
    CHECK(memcmp(frame, GOLDEN_STATUS_REQUEST, sizeof(GOLDEN_STATUS_REQUEST)) == 0);
    CHECK(nrf_log_protocol_decode_status_request(frame, sizeof(GOLDEN_STATUS_REQUEST), &decoded_status_request) == NRF_LOG_ERROR_OK);
    CHECK(decoded_status_request.request_id == status_request.request_id);

    CHECK(nrf_log_protocol_encode_status(&status, frame) == NRF_LOG_ERROR_OK);
    CHECK(memcmp(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS)) == 0);
    CHECK(nrf_log_protocol_decode_status(GOLDEN_STATUS, sizeof(GOLDEN_STATUS), &decoded_status) == NRF_LOG_ERROR_OK);
    CHECK(nrf_log_protocol_encode_status(&decoded_status, frame) == NRF_LOG_ERROR_OK);
    CHECK(memcmp(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS)) == 0);

    CHECK(nrf_log_protocol_encode_block(&block, frame, &length) == NRF_LOG_ERROR_OK);
    CHECK(length == sizeof(GOLDEN_BLOCK));
    CHECK(memcmp(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK)) == 0);
    CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), &validation, &decoded_block) == NRF_LOG_ERROR_OK);
    CHECK(decoded_block.record_count == 5U);
    CHECK(nrf_log_protocol_encode_block(&decoded_block, frame, &length) == NRF_LOG_ERROR_OK);
    CHECK(length == sizeof(GOLDEN_BLOCK));
    CHECK(memcmp(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK)) == 0);
}

static void test_request_boundaries_and_record_enums(void)
{
    nrf_log_read_request_t request;
    nrf_log_read_request_t decoded;
    nrf_log_record_t record;
    nrf_log_record_t decoded_record;
    uint8_t frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    uint8_t bytes[NRF_LOG_PROTOCOL_RECORD_LENGTH];
    uint8_t severity;
    uint8_t subsystem;

    memset(&request, 0, sizeof(request));
    request.request_id = 0x11223344U;
    request.expected_boot_id = 0x55667788U;
    request.cursor_seq = 0xAABBCCDDU;
    request.max_records = 1U;
    CHECK(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_OK);
    CHECK(frame[18] == 1U && frame[19] == 0U);
    CHECK(nrf_log_protocol_decode_read_request(frame, sizeof(frame), &decoded) == NRF_LOG_ERROR_OK);
    CHECK(decoded.request_id == request.request_id && decoded.expected_boot_id == request.expected_boot_id);
    CHECK(decoded.cursor_seq == request.cursor_seq && decoded.max_records == 1U);
    request.max_records = NRF_LOG_PROTOCOL_MAX_RECORDS;
    CHECK(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_OK);
    CHECK(nrf_log_protocol_decode_read_request(frame, sizeof(frame), &decoded) == NRF_LOG_ERROR_OK);
    CHECK(decoded.max_records == NRF_LOG_PROTOCOL_MAX_RECORDS);
    request.max_records = 0U;
    CHECK(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_COUNT_OVERFLOW);
    request.max_records = (uint8_t)(NRF_LOG_PROTOCOL_MAX_RECORDS + 1U);
    CHECK(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_COUNT_OVERFLOW);

    record.seq = UINT32_MAX;
    record.uptime_ms = UINT32_MAX;
    record.event_id = 0xFFFFU;
    record.arg0 = UINT32_MAX;
    record.arg1 = 0x80000000U;
    for (severity = NRF_LOG_SEVERITY_DEBUG; severity <= NRF_LOG_SEVERITY_FATAL; ++severity) {
        record.severity = severity;
        for (subsystem = NRF_LOG_SUBSYSTEM_BOOT; subsystem <= NRF_LOG_SUBSYSTEM_POWER; ++subsystem) {
            record.subsystem = subsystem;
            CHECK(nrf_log_protocol_encode_record(&record, bytes) == NRF_LOG_ERROR_OK);
            CHECK(nrf_log_protocol_decode_record(bytes, &decoded_record) == NRF_LOG_ERROR_OK);
            CHECK(decoded_record.seq == UINT32_MAX && decoded_record.event_id == 0xFFFFU);
            CHECK(decoded_record.severity == severity && decoded_record.subsystem == subsystem);
        }
        record.subsystem = 0xFEU;
        CHECK(nrf_log_protocol_encode_record(&record, bytes) == NRF_LOG_ERROR_OK);
        CHECK(nrf_log_protocol_decode_record(bytes, &decoded_record) == NRF_LOG_ERROR_OK);
    }
    record.severity = 5U;
    record.subsystem = NRF_LOG_SUBSYSTEM_BOOT;
    CHECK(nrf_log_protocol_encode_record(&record, bytes) == NRF_LOG_ERROR_BAD_SEVERITY);
    record.severity = NRF_LOG_SEVERITY_INFO;
    record.subsystem = NRF_LOG_SUBSYSTEM_INVALID;
    CHECK(nrf_log_protocol_encode_record(&record, bytes) == NRF_LOG_ERROR_BAD_SUBSYSTEM);
}

static void test_block_count_lengths(void)
{
    nrf_log_block_t block;
    nrf_log_block_t decoded;
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    uint8_t count;
    size_t length;
    memset(&block, 0, sizeof(block));
    block.request_id = 1U;
    block.boot_id = 1U;
    for (count = 0U; count <= NRF_LOG_PROTOCOL_MAX_RECORDS; ++count) {
        uint8_t i;
        block.record_count = count;
        block.first_seq = 20U;
        block.next_cursor = 20U + count;
        block.oldest_seq = 20U;
        for (i = 0U; i < count; ++i) {
            block.records[i].seq = 20U + i;
            block.records[i].uptime_ms = 100U + i;
            block.records[i].event_id = NRF_LOG_EVENT_LOG_TEST_EVENT;
            block.records[i].severity = NRF_LOG_SEVERITY_INFO;
            block.records[i].subsystem = NRF_LOG_SUBSYSTEM_LOG;
        }
        CHECK(nrf_log_protocol_encode_block(&block, frame, &length) == NRF_LOG_ERROR_OK);
        CHECK(length == NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH + (size_t)count * NRF_LOG_PROTOCOL_RECORD_LENGTH);
        CHECK(nrf_log_protocol_decode_block(frame, length, NULL, &decoded) == NRF_LOG_ERROR_OK);
    }
}

static void test_truncation_and_length_framing(void)
{
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U];
    uint8_t status_request_frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
    uint8_t read_request_frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
    nrf_log_status_request_t status_request_source = {0x01020304U};
    nrf_log_read_request_t read_request_source = {0x01020304U, 1U, 0U, NRF_LOG_PROTOCOL_MAX_RECORDS};
    nrf_log_status_request_t status_request;
    nrf_log_read_request_t read_request;
    nrf_log_status_t status;
    nrf_log_block_t block;
    guarded_block_t guarded;
    size_t length;

    CHECK(nrf_log_protocol_encode_status_request(&status_request_source, status_request_frame) == NRF_LOG_ERROR_OK);
    CHECK(nrf_log_protocol_encode_read_request(&read_request_source, read_request_frame) == NRF_LOG_ERROR_OK);
    for (length = 0U; length < NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH; ++length) {
        CHECK(nrf_log_protocol_decode_status_request(status_request_frame, length, &status_request) != NRF_LOG_ERROR_OK);
    }
    for (length = 0U; length < NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH; ++length) {
        CHECK(nrf_log_protocol_decode_read_request(read_request_frame, length, &read_request) != NRF_LOG_ERROR_OK);
    }
    for (length = 0U; length < NRF_LOG_PROTOCOL_STATUS_LENGTH; ++length) {
        CHECK(nrf_log_protocol_decode_status(GOLDEN_STATUS, length, &status) != NRF_LOG_ERROR_OK);
    }
    for (length = 0U; length < NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH; ++length) {
        CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, length, NULL, &block) != NRF_LOG_ERROR_OK);
    }
    memset(&guarded, 0xA5, sizeof(guarded));
    CHECK(nrf_log_protocol_decode_block(
        GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), NULL, &guarded.block) == NRF_LOG_ERROR_OK);
    check_bytes(guarded.before, sizeof(guarded.before), 0xA5U);
    check_bytes(guarded.after, sizeof(guarded.after), 0xA5U);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[12] = (uint8_t)(NRF_LOG_PROTOCOL_MAX_RECORDS + 1U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    memset(&guarded, 0xA5, sizeof(guarded));
    CHECK(nrf_log_protocol_decode_block(
        frame, sizeof(GOLDEN_BLOCK), NULL, &guarded.block) == NRF_LOG_ERROR_COUNT_OVERFLOW);
    check_bytes(guarded.before, sizeof(guarded.before), 0xA5U);
    check_bytes(guarded.after, sizeof(guarded.after), 0xA5U);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH] = 0U;
    set_le16(&frame[4], (uint16_t)(NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U));
    refresh_crc(frame, NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U);
    CHECK(nrf_log_protocol_decode_block(frame, NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U, NULL, &block) == NRF_LOG_ERROR_BAD_LENGTH);

    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status) == NRF_LOG_ERROR_OK);
    set_le16(&frame[4], (uint16_t)(sizeof(GOLDEN_STATUS) - 1U));
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status) == NRF_LOG_ERROR_BAD_LENGTH);
}

static void test_semantic_error_codes(void)
{
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    nrf_log_status_t status_out = {0};
    nrf_log_block_t block_out;
    size_t length;
    nrf_log_block_validation_t validation = {true, 0xA1B2C3D4U, true, 5U, true, 5U, true, 10U};

    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    frame[0] ^= 0x01U;
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_BAD_MAGIC);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    frame[1] ^= 0x01U;
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_BAD_MAGIC);

    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    frame[10] = 2U;
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_BAD_VERSION);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    frame[11] = 0x80U;
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_RESERVED_NONZERO);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    set_le16(&frame[12], 1U);
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    set_le16(&frame[14], 3U);
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    set_le32(&frame[20], 10U);
    set_le32(&frame[24], 8U);
    set_le16(&frame[14], 0U);
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    set_le32(&frame[16], 0U);
    refresh_crc(frame, sizeof(GOLDEN_STATUS));
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status_out) == NRF_LOG_ERROR_BAD_BOOT_ID);

    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[0] ^= 0x01U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_MAGIC);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[1] ^= 0x01U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_MAGIC);

    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[10] = 2U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_VERSION);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[44] = 5U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_SEVERITY);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[44] = 0x85U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_RESERVED_NONZERO);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[45] = 0U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_SUBSYSTEM);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[11] = 0x08U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_RESERVED_NONZERO);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[11] = NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[13] = 1U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_RESERVED_NONZERO);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le32(&frame[26], 6U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[12] = 6U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_COUNT_OVERFLOW);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[12] = 4U;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_LENGTH);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le16(&frame[4], (uint16_t)(sizeof(GOLDEN_BLOCK) - NRF_LOG_PROTOCOL_RECORD_LENGTH));
    refresh_crc(frame, sizeof(GOLDEN_BLOCK) - NRF_LOG_PROTOCOL_RECORD_LENGTH);
    CHECK(nrf_log_protocol_decode_block(
        frame,
        sizeof(GOLDEN_BLOCK) - NRF_LOG_PROTOCOL_RECORD_LENGTH,
        NULL,
        &block_out) == NRF_LOG_ERROR_BAD_LENGTH);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le32(&frame[14], 0U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_BAD_BOOT_ID);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le32(&frame[54], 99U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_SEQ_MISMATCH);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le32(&frame[54], 4U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_SEQ_MISMATCH);
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    set_le32(&frame[58], 50U);
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block_out) == NRF_LOG_ERROR_UPTIME_REGRESSION);
    CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), &validation, &block_out) == NRF_LOG_ERROR_OK);
    validation.max_records = 4U;
    CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), &validation, &block_out) == NRF_LOG_ERROR_COUNT_OVERFLOW);
    validation.max_records = 5U;
    validation.cursor_seq = 4U;
    CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), &validation, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    validation.cursor_seq = 5U;
    validation.status_next_seq = 9U;
    CHECK(nrf_log_protocol_decode_block(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK), &validation, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    validation.status_next_seq = 10U;
    validation.cursor_seq = 5U;
    validation.expected_boot_id = 0xA1B2C3D4U;
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[11] = NRF_LOG_BLOCK_FLAG_GAP;
    refresh_crc(frame, sizeof(GOLDEN_BLOCK));
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), &validation, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);

    memset(&block_out, 0, sizeof(block_out));
    block_out.boot_id = 1U;
    block_out.first_seq = 0U;
    block_out.next_cursor = 0U;
    block_out.oldest_seq = 0U;
    CHECK(nrf_log_protocol_encode_block(&block_out, frame, &length) == NRF_LOG_ERROR_OK);
    frame[11] = NRF_LOG_BLOCK_FLAG_MORE;
    refresh_crc(frame, length);
    CHECK(nrf_log_protocol_decode_block(frame, length, NULL, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);
    frame[11] = 0U;
    refresh_crc(frame, length);
    validation.has_expected_boot_id = true;
    validation.expected_boot_id = 1U;
    validation.has_cursor = true;
    validation.cursor_seq = 0U;
    validation.has_max_records = true;
    validation.max_records = 5U;
    validation.has_status_next_seq = true;
    validation.status_next_seq = 1U;
    CHECK(nrf_log_protocol_decode_block(frame, length, &validation, &block_out) == NRF_LOG_ERROR_INVARIANT_VIOLATION);

    status_out.flags = 0x04U;
    status_out.capacity = 0U;
    status_out.available = 0U;
    status_out.boot_id = 1U;
    status_out.oldest_seq = 0U;
    status_out.next_seq = 0U;
    CHECK(nrf_log_protocol_encode_status(&status_out, frame) == NRF_LOG_ERROR_RESERVED_NONZERO);

    {
        nrf_log_read_request_t request = {1U, 2U, 3U, 1U};
        nrf_log_read_request_t decoded;
        CHECK(nrf_log_protocol_encode_read_request(&request, frame) == NRF_LOG_ERROR_OK);
        frame[18] = 0U;
        refresh_crc(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH);
        CHECK(nrf_log_protocol_decode_read_request(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH, &decoded) == NRF_LOG_ERROR_COUNT_OVERFLOW);
        frame[18] = 6U;
        refresh_crc(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH);
        CHECK(nrf_log_protocol_decode_read_request(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH, &decoded) == NRF_LOG_ERROR_COUNT_OVERFLOW);
        frame[18] = 1U;
        frame[19] = 1U;
        refresh_crc(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH);
        CHECK(nrf_log_protocol_decode_read_request(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH, &decoded) == NRF_LOG_ERROR_RESERVED_NONZERO);
    }
}

static void test_corruption_rejection(void)
{
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
    size_t length;
    size_t byte_index;
    unsigned int bit;
    nrf_log_status_t status;
    nrf_log_block_t block;

    for (byte_index = 0U; byte_index < sizeof(GOLDEN_STATUS); ++byte_index) {
        for (bit = 0U; bit < 8U; ++bit) {
            memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
            frame[byte_index] ^= (uint8_t)(1U << bit);
            CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status) != NRF_LOG_ERROR_OK);
        }
    }
    for (byte_index = 0U; byte_index < sizeof(GOLDEN_BLOCK); ++byte_index) {
        for (bit = 0U; bit < 8U; ++bit) {
            memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
            frame[byte_index] ^= (uint8_t)(1U << bit);
            CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block) != NRF_LOG_ERROR_OK);
        }
    }
    check_burst_corruption_rejected(GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    check_burst_corruption_rejected(GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    for (length = sizeof(GOLDEN_STATUS); length <= sizeof(GOLDEN_BLOCK); length += sizeof(GOLDEN_BLOCK) - sizeof(GOLDEN_STATUS)) {
        size_t pattern;
        for (pattern = 0U; pattern < 4U; ++pattern) {
            size_t i;
            for (i = 0U; i < length; ++i) {
                frame[i] = pattern == 0U ? 0x00U :
                    (pattern == 1U ? 0xFFU :
                    (pattern == 2U ? 0xAAU : (i % 2U == 0U ? 0xAAU : 0x55U)));
            }
            if (length == sizeof(GOLDEN_STATUS)) {
                CHECK(nrf_log_protocol_decode_status(frame, length, &status) != NRF_LOG_ERROR_OK);
            } else {
                CHECK(nrf_log_protocol_decode_block(frame, length, NULL, &block) != NRF_LOG_ERROR_OK);
            }
        }
    }
    memcpy(frame, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
    frame[4] = GOLDEN_BLOCK[5];
    frame[5] = GOLDEN_BLOCK[4];
    CHECK(nrf_log_protocol_decode_block(frame, sizeof(GOLDEN_BLOCK), NULL, &block) != NRF_LOG_ERROR_OK);
    memcpy(frame, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
    frame[36] = GOLDEN_STATUS[37];
    frame[37] = GOLDEN_STATUS[36];
    CHECK(nrf_log_protocol_decode_status(frame, sizeof(GOLDEN_STATUS), &status) != NRF_LOG_ERROR_OK);
}

static uint32_t random_next(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void test_seeded_arbitrary_frames(void)
{
    uint8_t bytes[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH + 1U];
    uint32_t state = 0x308C0FFEU;
    unsigned int iteration;
    unsigned int successes = 0U;

    for (iteration = 0U; iteration < 10000U; ++iteration) {
        size_t length;
        size_t i;
        uint8_t type;
        active_seed = state;
        length = random_next(&state) % (sizeof(bytes) + 1U);
        for (i = 0U; i < sizeof(bytes); ++i) {
            bytes[i] = (uint8_t)random_next(&state);
        }
        if (iteration == 0U) {
            memcpy(bytes, GOLDEN_STATUS, sizeof(GOLDEN_STATUS));
            length = sizeof(GOLDEN_STATUS);
        } else if (iteration == 1U) {
            memcpy(bytes, GOLDEN_BLOCK, sizeof(GOLDEN_BLOCK));
            length = sizeof(GOLDEN_BLOCK);
        }
        type = length > 3U ? bytes[3] : 0xFFU;
        if (type == NRF_LOG_MESSAGE_STATUS_REQUEST) {
            nrf_log_status_request_t decoded;
            uint8_t encoded[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH];
            if (nrf_log_protocol_decode_status_request(bytes, length, &decoded) == NRF_LOG_ERROR_OK) {
                CHECK(nrf_log_protocol_encode_status_request(&decoded, encoded) == NRF_LOG_ERROR_OK);
                CHECK(length == sizeof(encoded) && memcmp(bytes, encoded, length) == 0);
                ++successes;
            }
        } else if (type == NRF_LOG_MESSAGE_READ_REQUEST) {
            nrf_log_read_request_t decoded;
            uint8_t encoded[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH];
            if (nrf_log_protocol_decode_read_request(bytes, length, &decoded) == NRF_LOG_ERROR_OK) {
                CHECK(nrf_log_protocol_encode_read_request(&decoded, encoded) == NRF_LOG_ERROR_OK);
                CHECK(length == sizeof(encoded) && memcmp(bytes, encoded, length) == 0);
                ++successes;
            }
        } else if (type == NRF_LOG_MESSAGE_STATUS) {
            nrf_log_status_t decoded;
            uint8_t encoded[NRF_LOG_PROTOCOL_STATUS_LENGTH];
            if (nrf_log_protocol_decode_status(bytes, length, &decoded) == NRF_LOG_ERROR_OK) {
                CHECK(nrf_log_protocol_encode_status(&decoded, encoded) == NRF_LOG_ERROR_OK);
                CHECK(length == sizeof(encoded) && memcmp(bytes, encoded, length) == 0);
                ++successes;
            }
        } else if (type == NRF_LOG_MESSAGE_BLOCK) {
            nrf_log_block_t decoded;
            uint8_t encoded[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH];
            size_t encoded_length = 0U;
            if (nrf_log_protocol_decode_block(bytes, length, NULL, &decoded) == NRF_LOG_ERROR_OK) {
                CHECK(nrf_log_protocol_encode_block(&decoded, encoded, &encoded_length) == NRF_LOG_ERROR_OK);
                CHECK(length == encoded_length && memcmp(bytes, encoded, length) == 0);
                ++successes;
            }
        }
    }
    active_seed = 0x308C0FFEU;
    printf("seeded arbitrary frame roundtrips: %u\n", successes);
}

int main(void)
{
    test_golden_vectors_and_roundtrip();
    test_request_boundaries_and_record_enums();
    test_block_count_lengths();
    test_truncation_and_length_framing();
    test_semantic_error_codes();
    test_corruption_rejection();
    test_seeded_arbitrary_frames();
    printf("nrf_log_protocol_test: PASS (%u checks, seed 0x308C0FFE)\n", checks);
    return 0;
}
