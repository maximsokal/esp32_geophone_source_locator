#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "nrf_fault_record.h"

static void test_fault_record_round_trip_and_checksum(void)
{
    const nrf_fault_record_t input = {
        .reset_cause = 0x12345678U,
        .cfsr = 0xA1B2C3D4U,
        .mmfsr = NRF_FAULT_RECORD_MMARVALID,
        .fault_flags = NRF_FAULT_RECORD_FLAG_TERMINAL,
        .fault_class = NRF_FAULT_CLASS_STACK_EXHAUSTION,
        .mmfar = 0x20001234U,
        .stacked_pc = 0x00012345U,
        .stacked_lr = 0x0006789AU,
        .thread_id = 0x10203040U,
        .thread_name = "sensor_worker",
        .boot_id = 0x55667788U,
        .firmware_version = "Oct 16 2026 12:34:56",
    };
    uint8_t bytes[NRF_FAULT_RECORD_ENCODED_SIZE];
    nrf_fault_record_t decoded;

    assert(nrf_fault_record_encode(&input, bytes, sizeof(bytes)) == NRF_FAULT_RECORD_OK);
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &decoded) == NRF_FAULT_RECORD_OK);
    assert(decoded.reset_cause == input.reset_cause);
    assert(decoded.cfsr == input.cfsr);
    assert(decoded.mmfsr == input.mmfsr);
    assert(decoded.fault_flags == input.fault_flags);
    assert(decoded.fault_class == input.fault_class);
    assert(decoded.mmfar == input.mmfar);
    assert(decoded.stacked_pc == input.stacked_pc);
    assert(decoded.stacked_lr == input.stacked_lr);
    assert(decoded.thread_id == input.thread_id);
    assert(strcmp(decoded.thread_name, input.thread_name) == 0);
    assert(decoded.boot_id == input.boot_id);
    assert(strcmp(decoded.firmware_version, input.firmware_version) == 0);

    bytes[NRF_FAULT_RECORD_ENCODED_SIZE - 1U] ^= 1U;
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &decoded) == NRF_FAULT_RECORD_BAD_CHECKSUM);
}

static void test_fault_class_prioritizes_stack_over_interrupt_context(void)
{
    assert(nrf_fault_record_classify(false, true) == NRF_FAULT_CLASS_STACK_EXHAUSTION);
    assert(nrf_fault_record_classify(true, true) == NRF_FAULT_CLASS_STACK_EXHAUSTION);
    assert(nrf_fault_record_classify(true, false) == NRF_FAULT_CLASS_INTERRUPT_CONTEXT);
    assert(nrf_fault_record_classify(false, false) == NRF_FAULT_CLASS_THREAD_CONTEXT);
}

static void test_mmfar_is_zero_without_mmarvalid(void)
{
    nrf_fault_record_t input = {0};
    nrf_fault_record_t decoded;
    uint8_t bytes[NRF_FAULT_RECORD_ENCODED_SIZE];

    input.mmfsr = 0x40U;
    input.mmfar = 0xFFFFFFFFU;
    assert(nrf_fault_record_encode(&input, bytes, sizeof(bytes)) == NRF_FAULT_RECORD_OK);
    assert(bytes[20] == 0U && bytes[21] == 0U && bytes[22] == 0U && bytes[23] == 0U);
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &decoded) == NRF_FAULT_RECORD_OK);
    assert(decoded.mmfar == 0U);

    input.mmfsr = NRF_FAULT_RECORD_MMARVALID;
    assert(nrf_fault_record_encode(&input, bytes, sizeof(bytes)) == NRF_FAULT_RECORD_OK);
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &decoded) == NRF_FAULT_RECORD_OK);
    assert(decoded.mmfar == UINT32_MAX);
}

static void test_fault_record_rejects_bad_arguments_and_header(void)
{
    uint8_t bytes[NRF_FAULT_RECORD_ENCODED_SIZE] = {0};
    nrf_fault_record_t record = {0};

    assert(nrf_fault_record_encode(NULL, bytes, sizeof(bytes)) == NRF_FAULT_RECORD_NULL_ARGUMENT);
    assert(nrf_fault_record_encode(&record, bytes, sizeof(bytes) - 1U) == NRF_FAULT_RECORD_BAD_LENGTH);
    bytes[6] = (uint8_t)NRF_FAULT_RECORD_ENCODED_SIZE;
    bytes[7] = 0U;
    assert(nrf_fault_record_decode(bytes, sizeof(bytes), &record) == NRF_FAULT_RECORD_BAD_MAGIC);
}

int main(void)
{
    test_fault_record_round_trip_and_checksum();
    test_fault_class_prioritizes_stack_over_interrupt_context();
    test_mmfar_is_zero_without_mmarvalid();
    test_fault_record_rejects_bad_arguments_and_header();
    return 0;
}
