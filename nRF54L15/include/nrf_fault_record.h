#ifndef NRF_FAULT_RECORD_H
#define NRF_FAULT_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define NRF_FAULT_RECORD_SCHEMA_VERSION 1U
#define NRF_FAULT_RECORD_THREAD_NAME_BYTES 16U
#define NRF_FAULT_RECORD_FIRMWARE_VERSION_BYTES 24U
#define NRF_FAULT_RECORD_ENCODED_SIZE 84U
#define NRF_FAULT_RECORD_MMARVALID 0x80U

typedef struct {
    uint32_t reset_cause;
    uint32_t cfsr;
    uint8_t mmfsr;
    uint32_t mmfar;
    uint32_t stacked_pc;
    uint32_t stacked_lr;
    uint32_t thread_id;
    char thread_name[NRF_FAULT_RECORD_THREAD_NAME_BYTES];
    uint32_t boot_id;
    char firmware_version[NRF_FAULT_RECORD_FIRMWARE_VERSION_BYTES];
} nrf_fault_record_t;

typedef enum {
    NRF_FAULT_RECORD_OK = 0,
    NRF_FAULT_RECORD_NULL_ARGUMENT,
    NRF_FAULT_RECORD_BAD_LENGTH,
    NRF_FAULT_RECORD_BAD_MAGIC,
    NRF_FAULT_RECORD_BAD_VERSION,
    NRF_FAULT_RECORD_BAD_CHECKSUM,
} nrf_fault_record_result_t;

nrf_fault_record_result_t nrf_fault_record_encode(
    const nrf_fault_record_t *record,
    uint8_t *buffer,
    size_t capacity);

nrf_fault_record_result_t nrf_fault_record_decode(
    const uint8_t *buffer,
    size_t length,
    nrf_fault_record_t *record);

#endif
