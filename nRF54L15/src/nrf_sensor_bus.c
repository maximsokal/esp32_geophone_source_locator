#include "nrf_sensor_bus.h"

#include <errno.h>

#define NRF_SENSOR_BUS_FAULT_CAUSE_SHIFT 8U
#define NRF_SENSOR_BUS_FAULT_ERRNO_SHIFT 16U
#define NRF_SENSOR_BUS_FAULT_SDA_BIT (UINT32_C(1) << 24)
#define NRF_SENSOR_BUS_FAULT_SCL_BIT (UINT32_C(1) << 25)
#define NRF_SENSOR_BUS_FAULT_VALID_BIT (UINT32_C(1) << 26)
#define NRF_SENSOR_BUS_FAULT_COUNT_SHIFT 27U
#define NRF_SENSOR_BUS_FAULT_COUNT_MAX 31U

nrf_sensor_bus_result_t nrf_sensor_bus_classify(int transfer_result)
{
    if (transfer_result == 0) {
        return NRF_SENSOR_BUS_OK;
    }
    if (transfer_result == -ETIMEDOUT) {
        return NRF_SENSOR_BUS_ERR_TIMEOUT;
    }
    if (transfer_result == -EIO || transfer_result == -EFAULT || transfer_result == -EAGAIN) {
        return NRF_SENSOR_BUS_ERR_IO;
    }
    return NRF_SENSOR_BUS_ERR_TRANSFER;
}

static uint32_t nrf_sensor_bus_fault_cause_code(nrf_sensor_bus_result_t cause)
{
    switch (cause) {
    case NRF_SENSOR_BUS_ERR_TIMEOUT: return 1U;
    case NRF_SENSOR_BUS_ERR_IO: return 2U;
    case NRF_SENSOR_BUS_ERR_TRANSFER: return 3U;
    default: return 0U;
    }
}

uint32_t nrf_sensor_bus_fault_pack(const nrf_sensor_bus_fault_t *fault)
{
    if (fault == NULL) {
        return 0U;
    }
    const uint32_t failures = fault->failures > NRF_SENSOR_BUS_FAULT_COUNT_MAX
        ? NRF_SENSOR_BUS_FAULT_COUNT_MAX : fault->failures;
    uint32_t packed = (uint32_t)fault->address |
        (nrf_sensor_bus_fault_cause_code(fault->cause) << NRF_SENSOR_BUS_FAULT_CAUSE_SHIFT) |
        ((uint32_t)fault->errno_abs << NRF_SENSOR_BUS_FAULT_ERRNO_SHIFT) |
        (failures << NRF_SENSOR_BUS_FAULT_COUNT_SHIFT);
    if (fault->lines_valid) {
        packed |= NRF_SENSOR_BUS_FAULT_VALID_BIT;
        if (fault->sda_high) packed |= NRF_SENSOR_BUS_FAULT_SDA_BIT;
        if (fault->scl_high) packed |= NRF_SENSOR_BUS_FAULT_SCL_BIT;
    }
    return packed;
}

bool nrf_sensor_bus_fault_unpack(uint32_t packed, nrf_sensor_bus_fault_t *out)
{
    static const nrf_sensor_bus_result_t causes[] = {
        NRF_SENSOR_BUS_ERR_TIMEOUT, NRF_SENSOR_BUS_ERR_IO, NRF_SENSOR_BUS_ERR_TRANSFER,
    };
    const uint32_t code = (packed >> NRF_SENSOR_BUS_FAULT_CAUSE_SHIFT) & 0xFFU;
    if (out == NULL || code == 0U || code > sizeof(causes) / sizeof(causes[0])) {
        return false;
    }
    const bool valid = (packed & NRF_SENSOR_BUS_FAULT_VALID_BIT) != 0U;
    out->address = (uint8_t)(packed & 0xFFU);
    out->cause = causes[code - 1U];
    out->errno_abs = (uint8_t)((packed >> NRF_SENSOR_BUS_FAULT_ERRNO_SHIFT) & 0xFFU);
    out->lines_valid = valid;
    out->sda_high = valid && (packed & NRF_SENSOR_BUS_FAULT_SDA_BIT) != 0U;
    out->scl_high = valid && (packed & NRF_SENSOR_BUS_FAULT_SCL_BIT) != 0U;
    out->failures = packed >> NRF_SENSOR_BUS_FAULT_COUNT_SHIFT;
    return true;
}

static int nrf_sensor_bus_fault_rank(const nrf_sensor_bus_fault_t *fault)
{
    if (fault->lines_valid && (!fault->sda_high || !fault->scl_high)) {
        return 4;
    }
    switch (fault->cause) {
    case NRF_SENSOR_BUS_ERR_TIMEOUT: return 3;
    case NRF_SENSOR_BUS_ERR_TRANSFER: return 2;
    case NRF_SENSOR_BUS_ERR_IO: return 1;
    default: return 0;
    }
}

bool nrf_sensor_bus_fault_outranks(const nrf_sensor_bus_fault_t *candidate, const nrf_sensor_bus_fault_t *current)
{
    if (candidate == NULL) {
        return false;
    }
    if (current == NULL) {
        return true;
    }
    const int candidate_rank = nrf_sensor_bus_fault_rank(candidate);
    const int current_rank = nrf_sensor_bus_fault_rank(current);
    if (candidate_rank != current_rank) {
        return candidate_rank > current_rank;
    }
    return candidate->address < current->address;
}

static nrf_sensor_bus_result_t nrf_sensor_bus_validate(
    const nrf_sensor_bus_t *bus,
    uint8_t address)
{
    if (bus == NULL || bus->transfer == NULL) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }
    if (!nrf_sensor_bus_is_valid_address(address)) {
        return NRF_SENSOR_BUS_ERR_ADDRESS;
    }
    return NRF_SENSOR_BUS_OK;
}

bool nrf_sensor_bus_is_valid_address(uint8_t address)
{
    return address >= 0x08U && address <= 0x77U;
}

nrf_sensor_bus_result_t nrf_sensor_bus_write(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *data,
    size_t data_len)
{
    const nrf_sensor_bus_result_t validation =
        nrf_sensor_bus_validate(bus, address);
    if (validation != NRF_SENSOR_BUS_OK) {
        return validation;
    }
    if (data == NULL || data_len == 0U) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }

    return nrf_sensor_bus_classify(bus->transfer(
        bus->context,
        address,
        data,
        data_len,
        NULL,
        0U));
}

nrf_sensor_bus_result_t nrf_sensor_bus_write_read(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len)
{
    const nrf_sensor_bus_result_t validation =
        nrf_sensor_bus_validate(bus, address);
    if (validation != NRF_SENSOR_BUS_OK) {
        return validation;
    }
    if (write_data == NULL || write_len == 0U ||
        read_data == NULL || read_len == 0U) {
        return NRF_SENSOR_BUS_ERR_ARGUMENT;
    }

    return nrf_sensor_bus_classify(bus->transfer(
        bus->context,
        address,
        write_data,
        write_len,
        read_data,
        read_len));
}
