#include "nrf_bme280.h"

/*
 * Deliberately its own translation unit, separate from nrf_bme280.c: this
 * function has no bus/hardware dependency (pure integer math), while
 * nrf_bme280.c pulls in nrf_sensor_bus's transfer functions. Keeping it
 * here lets callers that only need the pure conversion (host tests, other
 * upstream-payload builders) link it without the bus layer.
 */

#define NRF_BME280_SEA_LEVEL_PRESSURE_PA 101325
#define NRF_BME280_ALTITUDE_PA_PER_METER 12

int32_t nrf_bme280_pressure_to_altitude_mm(uint32_t pressure_pa)
{
    const int64_t delta_pa =
        (int64_t)NRF_BME280_SEA_LEVEL_PRESSURE_PA - (int64_t)pressure_pa;
    const int64_t altitude_mm =
        (delta_pa * 1000) / NRF_BME280_ALTITUDE_PA_PER_METER;

    if (altitude_mm > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (altitude_mm < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)altitude_mm;
}
