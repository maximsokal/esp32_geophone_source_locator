#ifndef NRF_BME280_H
#define NRF_BME280_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_sensor_bus.h"
#include "nrf_sensor_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NRF_BME280_ADDRESS_PRIMARY = 0x76U,
    NRF_BME280_ADDRESS_SECONDARY = 0x77U,
    NRF_BME280_CHIP_ID = 0x60U,
};

typedef enum {
    NRF_BME280_OK = 0,
    NRF_BME280_ERR_ARGUMENT = -1,
    NRF_BME280_ERR_BUS = -2,
    NRF_BME280_ERR_CHIP_ID = -3,
    NRF_BME280_ERR_NVM = -4,
    NRF_BME280_ERR_CALIBRATION = -5,
    NRF_BME280_ERR_CONFIG = -6,
    NRF_BME280_ERR_NOT_INITIALIZED = -7,
    NRF_BME280_ERR_MEASUREMENT = -8,
} nrf_bme280_result_t;

typedef void (*nrf_bme280_delay_ms_fn)(
    void *context,
    uint32_t delay_ms);

typedef struct {
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint16_t dig_p1;
    int16_t dig_p2;
    int16_t dig_p3;
    int16_t dig_p4;
    int16_t dig_p5;
    int16_t dig_p6;
    int16_t dig_p7;
    int16_t dig_p8;
    int16_t dig_p9;
    uint8_t dig_h1;
    int16_t dig_h2;
    uint8_t dig_h3;
    int16_t dig_h4;
    int16_t dig_h5;
    int8_t dig_h6;
} nrf_bme280_calibration_t;

typedef struct {
    nrf_sensor_bus_t bus;
    nrf_bme280_delay_ms_fn delay_ms;
    void *delay_context;
    nrf_bme280_calibration_t calibration;
    uint8_t address;
    bool initialized;
} nrf_bme280_t;

/*
 * Probe 0x76 then 0x77 and initialize an optional external BME280 at runtime.
 * Call only after runtime role assignment selects a REMOTE node. The shared
 * MAIN/REMOTE production image must not require this sensor during boot.
 */
nrf_bme280_result_t nrf_bme280_init(
    nrf_bme280_t *device,
    const nrf_sensor_bus_t *bus,
    nrf_bme280_delay_ms_fn delay_ms,
    void *delay_context);

/* Matches nrf_read_env_fn so it plugs directly into sensor snapshots. */
int nrf_bme280_read_env(void *context, nrf_env_sample_t *sample);

/*
 * Relative-altitude estimate from raw pressure, fixed-point only (no libm
 * dependency, matching nrf_orientation's convention). Uses the linear
 * approximation valid for small elevation ranges (~12 Pa/m near ground)
 * rather than the full ISA barometric formula: this project only needs
 * relative altitude between co-located nodes read close together in time,
 * not absolute MSL altitude, so the flat local-weather-driven offset in a
 * survey area cancels out when the caller subtracts one node's estimate
 * from another's.
 */
int32_t nrf_bme280_pressure_to_altitude_mm(uint32_t pressure_pa);

#ifdef __cplusplus
}
#endif

#endif
