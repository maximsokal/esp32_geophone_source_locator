#ifndef NRF_BMI160_H
#define NRF_BMI160_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_sensor_bus.h"
#include "nrf_sensor_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NRF_BMI160_DEFAULT_ADDRESS = 0x68U,
    NRF_BMI160_CHIP_ID = 0xD1U,
};

typedef enum {
    NRF_BMI160_OK = 0,
    NRF_BMI160_ERR_ARGUMENT = -1,
    NRF_BMI160_ERR_BUS = -2,
    NRF_BMI160_ERR_CHIP_ID = -3,
    NRF_BMI160_ERR_PMU = -4,
    NRF_BMI160_ERR_NOT_INITIALIZED = -5,
} nrf_bmi160_result_t;

typedef void (*nrf_bmi160_delay_ms_fn)(
    void *context,
    uint32_t delay_ms);

typedef struct {
    nrf_sensor_bus_t bus;
    nrf_bmi160_delay_ms_fn delay_ms;
    void *delay_context;
    uint8_t address;
    bool initialized;
} nrf_bmi160_t;

/*
 * Initialize an optional external BMI160 at runtime.
 *
 * The same firmware runs on MAIN and REMOTE XIAO nodes, while only REMOTE
 * hardware carries this sensor. Call this only after runtime role assignment
 * selects a REMOTE node; sensor absence is therefore not a boot-time error.
 */
nrf_bmi160_result_t nrf_bmi160_init(
    nrf_bmi160_t *device,
    const nrf_sensor_bus_t *bus,
    uint8_t address,
    nrf_bmi160_delay_ms_fn delay_ms,
    void *delay_context);

/* Matches nrf_read_imu_fn so it can plug directly into sensor snapshots. */
int nrf_bmi160_read_imu(void *context, nrf_imu_sample_t *sample);

#ifdef __cplusplus
}
#endif

#endif
