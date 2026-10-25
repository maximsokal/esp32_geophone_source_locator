#include "nrf_log_protocol.h"

#include "nrf_log_events.h"
#include "nrf_upstream_protocol.h"

#include <string.h>

#define NRF_LOG_MAGIC_0 0x47U
#define NRF_LOG_MAGIC_1 0x53U

const nrf_log_event_definition_t nrf_log_event_definitions[] = {
#define X(id, name, subsystem, severity) \
    { (uint16_t)(id), #name, NRF_LOG_SUBSYSTEM_##subsystem, NRF_LOG_SEVERITY_##severity },
#include "nrf_log_events.def"
#undef X
};

const size_t nrf_log_event_definition_count =
    sizeof(nrf_log_event_definitions) / sizeof(nrf_log_event_definitions[0]);

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U);
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
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static void write_header(uint8_t *frame, uint8_t type, uint16_t length, uint32_t request_id)
{
    frame[0] = NRF_LOG_MAGIC_0;
    frame[1] = NRF_LOG_MAGIC_1;
    frame[2] = NRF_LOG_PROTOCOL_VERSION;
    frame[3] = type;
    write_le16(&frame[4], length);
    write_le32(&frame[6], request_id);
}

static void finish_frame(uint8_t *frame, size_t length)
{
    write_le16(
        &frame[length - NRF_LOG_PROTOCOL_CRC_LENGTH],
        nrf_upstream_protocol_crc16_ccitt_false(frame, length - NRF_LOG_PROTOCOL_CRC_LENGTH));
}

static nrf_log_error_t validate_header(
    const uint8_t *frame,
    size_t length,
    uint8_t expected_type,
    size_t expected_length)
{
    if (frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (length < NRF_LOG_PROTOCOL_HEADER_LENGTH) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    if (frame[0] != NRF_LOG_MAGIC_0 || frame[1] != NRF_LOG_MAGIC_1) {
        return NRF_LOG_ERROR_BAD_MAGIC;
    }
    if (frame[2] != NRF_LOG_PROTOCOL_VERSION) {
        return NRF_LOG_ERROR_BAD_VERSION;
    }
    if (frame[3] != expected_type) {
        return NRF_LOG_ERROR_BAD_TYPE;
    }
    if (read_le16(&frame[4]) != length || (expected_length != 0U && length != expected_length)) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    return NRF_LOG_ERROR_OK;
}

static nrf_log_error_t validate_crc(const uint8_t *frame, size_t length)
{
    if (length < NRF_LOG_PROTOCOL_HEADER_LENGTH + NRF_LOG_PROTOCOL_CRC_LENGTH) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    if (nrf_upstream_protocol_crc16_ccitt_false(
            frame, length - NRF_LOG_PROTOCOL_CRC_LENGTH) !=
        read_le16(&frame[length - NRF_LOG_PROTOCOL_CRC_LENGTH])) {
        return NRF_LOG_ERROR_BAD_CRC;
    }
    return NRF_LOG_ERROR_OK;
}

static nrf_log_error_t validate_record(const nrf_log_record_t *record)
{
    if ((record->severity & 0xF8U) != 0U) {
        return NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (record->severity > NRF_LOG_SEVERITY_FATAL) {
        return NRF_LOG_ERROR_BAD_SEVERITY;
    }
    if (record->subsystem == NRF_LOG_SUBSYSTEM_INVALID) {
        return NRF_LOG_ERROR_BAD_SUBSYSTEM;
    }
    return NRF_LOG_ERROR_OK;
}

static nrf_log_error_t validate_status(const nrf_log_status_t *status)
{
    uint64_t sequence_count;

    if ((status->flags & (uint8_t)~NRF_LOG_STATUS_FLAG_MASK) != 0U) {
        return NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (status->boot_id == 0U) {
        return NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    if (status->next_seq < status->oldest_seq) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    sequence_count = (uint64_t)status->next_seq - status->oldest_seq;
    if (status->available > status->capacity || sequence_count != status->available) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    return NRF_LOG_ERROR_OK;
}

static nrf_log_error_t validate_block(
    const nrf_log_block_t *block,
    const nrf_log_block_validation_t *validation)
{
    uint64_t expected_next;
    bool more;
    bool gap;
    bool mismatch;
    uint8_t i;

    if ((block->flags & (uint8_t)~NRF_LOG_BLOCK_FLAG_MASK) != 0U) {
        return NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (block->record_count > NRF_LOG_PROTOCOL_MAX_RECORDS) {
        return NRF_LOG_ERROR_COUNT_OVERFLOW;
    }
    if (block->boot_id == 0U) {
        return NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    more = (block->flags & NRF_LOG_BLOCK_FLAG_MORE) != 0U;
    gap = (block->flags & NRF_LOG_BLOCK_FLAG_GAP) != 0U;
    mismatch = (block->flags & NRF_LOG_BLOCK_FLAG_BOOT_MISMATCH) != 0U;
    if (mismatch && block->record_count != 0U) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    if (more && block->record_count == 0U) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    if (block->first_seq < block->oldest_seq) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    expected_next = (uint64_t)block->first_seq + block->record_count;
    if (expected_next > UINT32_MAX || block->next_cursor != (uint32_t)expected_next) {
        return NRF_LOG_ERROR_INVARIANT_VIOLATION;
    }
    if (validation != NULL) {
        if (validation->has_expected_boot_id &&
            mismatch != (validation->expected_boot_id != block->boot_id)) {
            return NRF_LOG_ERROR_INVARIANT_VIOLATION;
        }
        if (validation->has_cursor) {
            if (gap && validation->cursor_seq >= block->oldest_seq) {
                return NRF_LOG_ERROR_INVARIANT_VIOLATION;
            }
            if (!gap && !mismatch && block->first_seq != validation->cursor_seq) {
                return NRF_LOG_ERROR_INVARIANT_VIOLATION;
            }
        }
        if (validation->has_max_records) {
            if (validation->max_records == 0U ||
                validation->max_records > NRF_LOG_PROTOCOL_MAX_RECORDS) {
                return NRF_LOG_ERROR_COUNT_OVERFLOW;
            }
            if (block->record_count > validation->max_records) {
                return NRF_LOG_ERROR_COUNT_OVERFLOW;
            }
            if (more && block->record_count < validation->max_records &&
                validation->has_status_next_seq &&
                block->next_cursor >= validation->status_next_seq) {
                return NRF_LOG_ERROR_INVARIANT_VIOLATION;
            }
        }
        if (validation->has_status_next_seq &&
            block->next_cursor > validation->status_next_seq) {
            return NRF_LOG_ERROR_INVARIANT_VIOLATION;
        }
        if (validation->has_status_next_seq && block->record_count == 0U &&
            block->first_seq != validation->status_next_seq) {
            return NRF_LOG_ERROR_INVARIANT_VIOLATION;
        }
    }
    for (i = 0U; i < block->record_count; ++i) {
        nrf_log_error_t result = validate_record(&block->records[i]);
        if (result != NRF_LOG_ERROR_OK) {
            return result;
        }
        if (block->records[i].seq != block->first_seq + i) {
            return NRF_LOG_ERROR_SEQ_MISMATCH;
        }
        if (i != 0U) {
            uint32_t previous = block->records[i - 1U].uptime_ms;
            uint32_t current = block->records[i].uptime_ms;
            if (current < previous && !(previous > 0xF0000000U && current < 0x10000000U)) {
                return NRF_LOG_ERROR_UPTIME_REGRESSION;
            }
        }
    }
    return NRF_LOG_ERROR_OK;
}

const nrf_log_event_definition_t *nrf_log_event_find(uint16_t id)
{
    size_t i;
    for (i = 0U; i < nrf_log_event_definition_count; ++i) {
        if (nrf_log_event_definitions[i].id == id) {
            return &nrf_log_event_definitions[i];
        }
    }
    return NULL;
}

nrf_log_error_t nrf_log_protocol_encode_record(
    const nrf_log_record_t *record,
    uint8_t bytes[NRF_LOG_PROTOCOL_RECORD_LENGTH])
{
    nrf_log_error_t result;
    if (record == NULL || bytes == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_record(record);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    write_le32(&bytes[0], record->seq);
    write_le32(&bytes[4], record->uptime_ms);
    write_le16(&bytes[8], record->event_id);
    bytes[10] = record->severity;
    bytes[11] = record->subsystem;
    write_le32(&bytes[12], record->arg0);
    write_le32(&bytes[16], record->arg1);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_record(
    const uint8_t bytes[NRF_LOG_PROTOCOL_RECORD_LENGTH],
    nrf_log_record_t *record)
{
    nrf_log_record_t decoded;
    nrf_log_error_t result;
    if (bytes == NULL || record == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    decoded.seq = read_le32(&bytes[0]);
    decoded.uptime_ms = read_le32(&bytes[4]);
    decoded.event_id = read_le16(&bytes[8]);
    decoded.severity = bytes[10];
    decoded.subsystem = bytes[11];
    decoded.arg0 = read_le32(&bytes[12]);
    decoded.arg1 = read_le32(&bytes[16]);
    result = validate_record(&decoded);
    if (result == NRF_LOG_ERROR_OK) {
        *record = decoded;
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_status_request(
    const nrf_log_status_request_t *request,
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH])
{
    if (request == NULL || frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    write_header(frame, NRF_LOG_MESSAGE_STATUS_REQUEST, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH, request->request_id);
    finish_frame(frame, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_status_request(
    const uint8_t *frame,
    size_t length,
    nrf_log_status_request_t *request)
{
    nrf_log_error_t result;
    nrf_log_status_request_t decoded;
    if (request == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_header(frame, length, NRF_LOG_MESSAGE_STATUS_REQUEST, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH);
    if (result == NRF_LOG_ERROR_OK) {
        result = validate_crc(frame, length);
    }
    if (result == NRF_LOG_ERROR_OK) {
        decoded.request_id = read_le32(&frame[6]);
        *request = decoded;
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_firmware_request(
    const nrf_log_status_request_t *request,
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH])
{
    if (request == NULL || frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    write_header(frame, NRF_LOG_MESSAGE_FIRMWARE_REQUEST,
        NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH, request->request_id);
    finish_frame(frame, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_firmware_request(
    const uint8_t *frame,
    size_t length,
    nrf_log_status_request_t *request)
{
    if (request == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    nrf_log_error_t result = validate_header(frame, length,
        NRF_LOG_MESSAGE_FIRMWARE_REQUEST, NRF_LOG_PROTOCOL_STATUS_REQUEST_LENGTH);
    if (result == NRF_LOG_ERROR_OK) {
        result = validate_crc(frame, length);
    }
    if (result == NRF_LOG_ERROR_OK) {
        request->request_id = read_le32(&frame[6]);
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_firmware(
    const nrf_log_firmware_info_t *info,
    uint8_t frame[NRF_LOG_PROTOCOL_FIRMWARE_LENGTH])
{
    if (info == NULL || frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (info->boot_id == 0U) {
        return NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    if ((info->flags & (uint8_t)~NRF_LOG_FIRMWARE_FLAG_MASK) != 0U) {
        return NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    write_header(frame, NRF_LOG_MESSAGE_FIRMWARE, NRF_LOG_PROTOCOL_FIRMWARE_LENGTH,
        info->request_id);
    frame[10] = info->flags;
    write_le32(&frame[11], info->boot_id);
    memcpy(&frame[15], info->git_commit, sizeof(info->git_commit));
    finish_frame(frame, NRF_LOG_PROTOCOL_FIRMWARE_LENGTH);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_firmware(
    const uint8_t *frame,
    size_t length,
    nrf_log_firmware_info_t *info)
{
    if (info == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    nrf_log_error_t result = validate_header(frame, length,
        NRF_LOG_MESSAGE_FIRMWARE, NRF_LOG_PROTOCOL_FIRMWARE_LENGTH);
    if (result == NRF_LOG_ERROR_OK) {
        result = validate_crc(frame, length);
    }
    if (result == NRF_LOG_ERROR_OK &&
        (frame[10] & (uint8_t)~NRF_LOG_FIRMWARE_FLAG_MASK) != 0U) {
        result = NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (result == NRF_LOG_ERROR_OK && read_le32(&frame[11]) == 0U) {
        result = NRF_LOG_ERROR_BAD_BOOT_ID;
    }
    if (result == NRF_LOG_ERROR_OK) {
        info->request_id = read_le32(&frame[6]);
        info->flags = frame[10];
        info->boot_id = read_le32(&frame[11]);
        memcpy(info->git_commit, &frame[15], sizeof(info->git_commit));
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_read_request(
    const nrf_log_read_request_t *request,
    uint8_t frame[NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH])
{
    if (request == NULL || frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    if (request->max_records == 0U || request->max_records > NRF_LOG_PROTOCOL_MAX_RECORDS) {
        return NRF_LOG_ERROR_COUNT_OVERFLOW;
    }
    write_header(frame, NRF_LOG_MESSAGE_READ_REQUEST, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH, request->request_id);
    write_le32(&frame[10], request->expected_boot_id);
    write_le32(&frame[14], request->cursor_seq);
    frame[18] = request->max_records;
    frame[19] = 0U;
    finish_frame(frame, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_read_request(
    const uint8_t *frame,
    size_t length,
    nrf_log_read_request_t *request)
{
    nrf_log_error_t result;
    nrf_log_read_request_t decoded;
    if (request == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_header(frame, length, NRF_LOG_MESSAGE_READ_REQUEST, NRF_LOG_PROTOCOL_READ_REQUEST_LENGTH);
    if (result == NRF_LOG_ERROR_OK) {
        result = validate_crc(frame, length);
    }
    if (result == NRF_LOG_ERROR_OK && frame[19] != 0U) {
        result = NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (result == NRF_LOG_ERROR_OK &&
        (frame[18] == 0U || frame[18] > NRF_LOG_PROTOCOL_MAX_RECORDS)) {
        result = NRF_LOG_ERROR_COUNT_OVERFLOW;
    }
    if (result == NRF_LOG_ERROR_OK) {
        decoded.request_id = read_le32(&frame[6]);
        decoded.expected_boot_id = read_le32(&frame[10]);
        decoded.cursor_seq = read_le32(&frame[14]);
        decoded.max_records = frame[18];
        *request = decoded;
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_status(
    const nrf_log_status_t *status,
    uint8_t frame[NRF_LOG_PROTOCOL_STATUS_LENGTH])
{
    nrf_log_error_t result;
    if (status == NULL || frame == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_status(status);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    write_header(frame, NRF_LOG_MESSAGE_STATUS, NRF_LOG_PROTOCOL_STATUS_LENGTH, status->request_id);
    frame[10] = NRF_LOG_PROTOCOL_VERSION;
    frame[11] = status->flags;
    write_le16(&frame[12], status->capacity);
    write_le16(&frame[14], status->available);
    write_le32(&frame[16], status->boot_id);
    write_le32(&frame[20], status->oldest_seq);
    write_le32(&frame[24], status->next_seq);
    write_le32(&frame[28], status->overwritten_total);
    write_le32(&frame[32], status->lost_internal_total);
    finish_frame(frame, NRF_LOG_PROTOCOL_STATUS_LENGTH);
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_status(
    const uint8_t *frame,
    size_t length,
    nrf_log_status_t *status)
{
    nrf_log_status_t decoded = {0};
    nrf_log_error_t result;
    if (status == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_header(frame, length, NRF_LOG_MESSAGE_STATUS, NRF_LOG_PROTOCOL_STATUS_LENGTH);
    if (result == NRF_LOG_ERROR_OK) {
        result = validate_crc(frame, length);
    }
    if (result == NRF_LOG_ERROR_OK && frame[10] != NRF_LOG_PROTOCOL_VERSION) {
        result = NRF_LOG_ERROR_BAD_VERSION;
    }
    if (result == NRF_LOG_ERROR_OK) {
        decoded.request_id = read_le32(&frame[6]);
        decoded.flags = frame[11];
        decoded.capacity = read_le16(&frame[12]);
        decoded.available = read_le16(&frame[14]);
        decoded.boot_id = read_le32(&frame[16]);
        decoded.oldest_seq = read_le32(&frame[20]);
        decoded.next_seq = read_le32(&frame[24]);
        decoded.overwritten_total = read_le32(&frame[28]);
        decoded.lost_internal_total = read_le32(&frame[32]);
        result = validate_status(&decoded);
    }
    if (result == NRF_LOG_ERROR_OK) {
        *status = decoded;
    }
    return result;
}

nrf_log_error_t nrf_log_protocol_encode_block(
    const nrf_log_block_t *block,
    uint8_t frame[NRF_LOG_PROTOCOL_MAX_BLOCK_LENGTH],
    size_t *length)
{
    nrf_log_error_t result;
    size_t encoded_length;
    size_t offset;
    uint8_t i;
    if (block == NULL || frame == NULL || length == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_block(block, NULL);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    encoded_length = NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
        (size_t)block->record_count * NRF_LOG_PROTOCOL_RECORD_LENGTH;
    write_header(frame, NRF_LOG_MESSAGE_BLOCK, (uint16_t)encoded_length, block->request_id);
    frame[10] = NRF_LOG_PROTOCOL_VERSION;
    frame[11] = block->flags;
    frame[12] = block->record_count;
    frame[13] = 0U;
    write_le32(&frame[14], block->boot_id);
    write_le32(&frame[18], block->first_seq);
    write_le32(&frame[22], block->next_cursor);
    write_le32(&frame[26], block->oldest_seq);
    write_le32(&frame[30], block->overwritten_total);
    offset = 34U;
    for (i = 0U; i < block->record_count; ++i) {
        result = nrf_log_protocol_encode_record(&block->records[i], &frame[offset]);
        if (result != NRF_LOG_ERROR_OK) {
            return result;
        }
        offset += NRF_LOG_PROTOCOL_RECORD_LENGTH;
    }
    finish_frame(frame, encoded_length);
    *length = encoded_length;
    return NRF_LOG_ERROR_OK;
}

nrf_log_error_t nrf_log_protocol_decode_block(
    const uint8_t *frame,
    size_t length,
    const nrf_log_block_validation_t *validation,
    nrf_log_block_t *block)
{
    nrf_log_block_t decoded;
    nrf_log_error_t result;
    size_t expected_length;
    size_t offset;
    uint8_t i;
    if (block == NULL) {
        return NRF_LOG_ERROR_NULL_ARGUMENT;
    }
    result = validate_header(frame, length, NRF_LOG_MESSAGE_BLOCK, 0U);
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    if (length < 13U) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    if (frame[12] > NRF_LOG_PROTOCOL_MAX_RECORDS) {
        return NRF_LOG_ERROR_COUNT_OVERFLOW;
    }
    expected_length = NRF_LOG_PROTOCOL_BLOCK_BASE_LENGTH +
        (size_t)frame[12] * NRF_LOG_PROTOCOL_RECORD_LENGTH;
    if (length != expected_length) {
        return NRF_LOG_ERROR_BAD_LENGTH;
    }
    result = validate_crc(frame, length);
    if (result == NRF_LOG_ERROR_OK && frame[10] != NRF_LOG_PROTOCOL_VERSION) {
        result = NRF_LOG_ERROR_BAD_VERSION;
    }
    if (result == NRF_LOG_ERROR_OK && frame[13] != 0U) {
        result = NRF_LOG_ERROR_RESERVED_NONZERO;
    }
    if (result != NRF_LOG_ERROR_OK) {
        return result;
    }
    memset(&decoded, 0, sizeof(decoded));
    decoded.request_id = read_le32(&frame[6]);
    decoded.flags = frame[11];
    decoded.record_count = frame[12];
    decoded.boot_id = read_le32(&frame[14]);
    decoded.first_seq = read_le32(&frame[18]);
    decoded.next_cursor = read_le32(&frame[22]);
    decoded.oldest_seq = read_le32(&frame[26]);
    decoded.overwritten_total = read_le32(&frame[30]);
    offset = 34U;
    for (i = 0U; i < decoded.record_count; ++i) {
        result = nrf_log_protocol_decode_record(&frame[offset], &decoded.records[i]);
        if (result != NRF_LOG_ERROR_OK) {
            return result;
        }
        offset += NRF_LOG_PROTOCOL_RECORD_LENGTH;
    }
    result = validate_block(&decoded, validation);
    if (result == NRF_LOG_ERROR_OK) {
        *block = decoded;
    }
    return result;
}
