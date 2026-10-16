#include "nrf_fault_record.h"

#include <string.h>

#define NRF_FAULT_RECORD_MAGIC UINT32_C(0x3146524E)
#define NRF_FAULT_RECORD_HEADER_LENGTH 8U
#define NRF_FAULT_RECORD_RESET_CAUSE_OFFSET 8U
#define NRF_FAULT_RECORD_CFSR_OFFSET 12U
#define NRF_FAULT_RECORD_MMFSR_OFFSET 16U
#define NRF_FAULT_RECORD_FLAGS_OFFSET 17U
#define NRF_FAULT_RECORD_CLASS_OFFSET 18U
#define NRF_FAULT_RECORD_MMFAR_OFFSET 20U
#define NRF_FAULT_RECORD_PC_OFFSET 24U
#define NRF_FAULT_RECORD_LR_OFFSET 28U
#define NRF_FAULT_RECORD_THREAD_ID_OFFSET 32U
#define NRF_FAULT_RECORD_THREAD_NAME_OFFSET 36U
#define NRF_FAULT_RECORD_BOOT_ID_OFFSET 52U
#define NRF_FAULT_RECORD_FIRMWARE_VERSION_OFFSET 56U
#define NRF_FAULT_RECORD_CHECKSUM_OFFSET 80U

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8U);
    destination[2] = (uint8_t)(value >> 16U);
    destination[3] = (uint8_t)(value >> 24U);
}

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8U));
}

static uint32_t read_u32(const uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8U) |
        ((uint32_t)source[2] << 16U) |
        ((uint32_t)source[3] << 24U);
}

static uint32_t checksum(const uint8_t *buffer, size_t length)
{
    uint32_t crc = UINT32_MAX;

    for (size_t index = 0U; index < length; ++index) {
        crc ^= buffer[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t low_bit_mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & low_bit_mask);
        }
    }
    return ~crc;
}

nrf_fault_class_t nrf_fault_record_classify(bool in_interrupt, bool stack_exhaustion)
{
    if (stack_exhaustion) {
        return NRF_FAULT_CLASS_STACK_EXHAUSTION;
    }
    return in_interrupt
        ? NRF_FAULT_CLASS_INTERRUPT_CONTEXT
        : NRF_FAULT_CLASS_THREAD_CONTEXT;
}

static void write_text(uint8_t *destination, size_t capacity, const char *text)
{
    if (text == NULL || capacity == 0U) {
        return;
    }
    for (size_t index = 0U; index + 1U < capacity && text[index] != '\0'; ++index) {
        destination[index] = (uint8_t)text[index];
    }
}

nrf_fault_record_result_t nrf_fault_record_encode(
    const nrf_fault_record_t *record,
    uint8_t *buffer,
    size_t capacity)
{
    if (record == NULL || buffer == NULL) {
        return NRF_FAULT_RECORD_NULL_ARGUMENT;
    }
    if (capacity != NRF_FAULT_RECORD_ENCODED_SIZE) {
        return NRF_FAULT_RECORD_BAD_LENGTH;
    }

    memset(buffer, 0, NRF_FAULT_RECORD_ENCODED_SIZE);
    write_u32(&buffer[0], NRF_FAULT_RECORD_MAGIC);
    write_u16(&buffer[4], NRF_FAULT_RECORD_SCHEMA_VERSION);
    write_u16(&buffer[6], NRF_FAULT_RECORD_ENCODED_SIZE);
    write_u32(&buffer[NRF_FAULT_RECORD_RESET_CAUSE_OFFSET], record->reset_cause);
    write_u32(&buffer[NRF_FAULT_RECORD_CFSR_OFFSET], record->cfsr);
    buffer[NRF_FAULT_RECORD_MMFSR_OFFSET] = record->mmfsr;
    buffer[NRF_FAULT_RECORD_FLAGS_OFFSET] = record->fault_flags;
    buffer[NRF_FAULT_RECORD_CLASS_OFFSET] = record->fault_class;
    if ((record->mmfsr & NRF_FAULT_RECORD_MMARVALID) != 0U) {
        write_u32(&buffer[NRF_FAULT_RECORD_MMFAR_OFFSET], record->mmfar);
    }
    write_u32(&buffer[NRF_FAULT_RECORD_PC_OFFSET], record->stacked_pc);
    write_u32(&buffer[NRF_FAULT_RECORD_LR_OFFSET], record->stacked_lr);
    write_u32(&buffer[NRF_FAULT_RECORD_THREAD_ID_OFFSET], record->thread_id);
    write_text(
        &buffer[NRF_FAULT_RECORD_THREAD_NAME_OFFSET],
        NRF_FAULT_RECORD_THREAD_NAME_BYTES,
        record->thread_name);
    write_u32(&buffer[NRF_FAULT_RECORD_BOOT_ID_OFFSET], record->boot_id);
    write_text(
        &buffer[NRF_FAULT_RECORD_FIRMWARE_VERSION_OFFSET],
        NRF_FAULT_RECORD_FIRMWARE_VERSION_BYTES,
        record->firmware_version);
    write_u32(
        &buffer[NRF_FAULT_RECORD_CHECKSUM_OFFSET],
        checksum(buffer, NRF_FAULT_RECORD_CHECKSUM_OFFSET));
    return NRF_FAULT_RECORD_OK;
}

nrf_fault_record_result_t nrf_fault_record_decode(
    const uint8_t *buffer,
    size_t length,
    nrf_fault_record_t *record)
{
    if (buffer == NULL || record == NULL) {
        return NRF_FAULT_RECORD_NULL_ARGUMENT;
    }
    if (length != NRF_FAULT_RECORD_ENCODED_SIZE ||
        read_u16(&buffer[6]) != NRF_FAULT_RECORD_ENCODED_SIZE) {
        return NRF_FAULT_RECORD_BAD_LENGTH;
    }
    if (read_u32(buffer) != NRF_FAULT_RECORD_MAGIC) {
        return NRF_FAULT_RECORD_BAD_MAGIC;
    }
    if (read_u16(&buffer[4]) != NRF_FAULT_RECORD_SCHEMA_VERSION) {
        return NRF_FAULT_RECORD_BAD_VERSION;
    }
    if (read_u32(&buffer[NRF_FAULT_RECORD_CHECKSUM_OFFSET]) !=
        checksum(buffer, NRF_FAULT_RECORD_CHECKSUM_OFFSET)) {
        return NRF_FAULT_RECORD_BAD_CHECKSUM;
    }

    memset(record, 0, sizeof(*record));
    record->reset_cause = read_u32(&buffer[NRF_FAULT_RECORD_RESET_CAUSE_OFFSET]);
    record->cfsr = read_u32(&buffer[NRF_FAULT_RECORD_CFSR_OFFSET]);
    record->mmfsr = buffer[NRF_FAULT_RECORD_MMFSR_OFFSET];
    record->fault_flags = buffer[NRF_FAULT_RECORD_FLAGS_OFFSET];
    record->fault_class = buffer[NRF_FAULT_RECORD_CLASS_OFFSET];
    if ((record->mmfsr & NRF_FAULT_RECORD_MMARVALID) != 0U) {
        record->mmfar = read_u32(&buffer[NRF_FAULT_RECORD_MMFAR_OFFSET]);
    }
    record->stacked_pc = read_u32(&buffer[NRF_FAULT_RECORD_PC_OFFSET]);
    record->stacked_lr = read_u32(&buffer[NRF_FAULT_RECORD_LR_OFFSET]);
    record->thread_id = read_u32(&buffer[NRF_FAULT_RECORD_THREAD_ID_OFFSET]);
    memcpy(record->thread_name, &buffer[NRF_FAULT_RECORD_THREAD_NAME_OFFSET],
           NRF_FAULT_RECORD_THREAD_NAME_BYTES);
    record->boot_id = read_u32(&buffer[NRF_FAULT_RECORD_BOOT_ID_OFFSET]);
    memcpy(record->firmware_version, &buffer[NRF_FAULT_RECORD_FIRMWARE_VERSION_OFFSET],
           NRF_FAULT_RECORD_FIRMWARE_VERSION_BYTES);
    record->thread_name[NRF_FAULT_RECORD_THREAD_NAME_BYTES - 1U] = '\0';
    record->firmware_version[NRF_FAULT_RECORD_FIRMWARE_VERSION_BYTES - 1U] = '\0';
    return NRF_FAULT_RECORD_OK;
}
