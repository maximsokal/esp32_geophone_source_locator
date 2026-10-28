#ifndef NRF_SENSOR_BUS_H
#define NRF_SENSOR_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NRF_SENSOR_BUS_OK = 0,
    NRF_SENSOR_BUS_ERR_ARGUMENT = -1,
    NRF_SENSOR_BUS_ERR_ADDRESS = -2,
    /* The transfer failed for a reason that is neither of the two below. */
    NRF_SENSOR_BUS_ERR_TRANSFER = -3,
    /* The transfer returned -ETIMEDOUT: the controller never completed (stuck or stretched line). */
    NRF_SENSOR_BUS_ERR_TIMEOUT = -4,
    /*
     * The transfer returned -EIO, -EFAULT or -EAGAIN. Zephyr's TWIM driver sees address NACK, data NACK and
     * overrun apart but returns -EIO for all of them, so "no device answered" and "the bus is broken" cannot be
     * told apart from this code alone; the line levels in nrf_sensor_bus_fault_t are what separates them.
     */
    NRF_SENSOR_BUS_ERR_IO = -5,
} nrf_sensor_bus_result_t;

/* Maps the raw value returned by a transfer function: 0 -> OK, otherwise TIMEOUT, IO or TRANSFER. */
nrf_sensor_bus_result_t nrf_sensor_bus_classify(int transfer_result);

typedef int (*nrf_sensor_bus_transfer_fn)(
    void *context,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len);

typedef struct {
    nrf_sensor_bus_transfer_fn transfer;
    void *context;
} nrf_sensor_bus_t;

bool nrf_sensor_bus_is_valid_address(uint8_t address);

/*
 * One failed transfer as it is reported upstream (SENSOR_BUS_FAULT, arg1). `failures` counts the failed transfers
 * folded into this sample; the line levels were read from the pins right after the failure.
 */
typedef struct {
    uint8_t address;
    nrf_sensor_bus_result_t cause; /* TIMEOUT, IO or TRANSFER */
    uint8_t errno_abs;             /* magnitude of the raw transfer result, saturated at 255 */
    bool lines_valid;
    bool sda_high;
    bool scl_high;
    uint32_t failures;
} nrf_sensor_bus_fault_t;

/*
 * Wire layout of SENSOR_BUS_FAULT arg1:
 *   bits 0-7   address              bits 8-15  cause (1 TIMEOUT, 2 IO, 3 TRANSFER)
 *   bits 16-23 errno magnitude      bit 24 SDA high, bit 25 SCL high, bit 26 line levels valid
 *   bits 27-31 failure count, saturated at 31
 */
uint32_t nrf_sensor_bus_fault_pack(const nrf_sensor_bus_fault_t *fault);
/* False for a NULL argument or a cause code that pack() never produces. */
bool nrf_sensor_bus_fault_unpack(uint32_t packed, nrf_sensor_bus_fault_t *out);
/*
 * Ordering used to pick the sample worth reporting: a line held low, then a timeout, then any other error,
 * then plain EIO. Equal rank is decided by the lower address; the same address never outranks itself.
 */
bool nrf_sensor_bus_fault_outranks(const nrf_sensor_bus_fault_t *candidate, const nrf_sensor_bus_fault_t *current);

nrf_sensor_bus_result_t nrf_sensor_bus_write(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *data,
    size_t data_len);

nrf_sensor_bus_result_t nrf_sensor_bus_write_read(
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    const uint8_t *write_data,
    size_t write_len,
    uint8_t *read_data,
    size_t read_len);

#ifdef __cplusplus
}
#endif

#endif
