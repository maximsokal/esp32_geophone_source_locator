#include "nrf_baro_altitude.h"

#include <limits.h>

#define NRF_BARO_SEA_LEVEL_PRESSURE_PA 101325
#define NRF_BARO_ALTITUDE_PA_PER_METER 12

int32_t nrf_baro_pressure_x100_to_altitude_mm(uint32_t pressure_x100_pa)
{
    /* The same model with the pressure in 0.01 Pa: a whole pascal is 83 mm of height, a hundredth 0.8 mm. */
    const int64_t delta_x100_pa =
        (int64_t)NRF_BARO_SEA_LEVEL_PRESSURE_PA * 100 - (int64_t)pressure_x100_pa;
    const int64_t altitude_mm = (delta_x100_pa * 10) / NRF_BARO_ALTITUDE_PA_PER_METER;

    if (altitude_mm > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (altitude_mm < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)altitude_mm;
}

int32_t nrf_baro_pressure_to_altitude_mm(uint32_t pressure_pa)
{
    const int64_t delta_pa =
        (int64_t)NRF_BARO_SEA_LEVEL_PRESSURE_PA - (int64_t)pressure_pa;
    const int64_t altitude_mm =
        (delta_pa * 1000) / NRF_BARO_ALTITUDE_PA_PER_METER;

    if (altitude_mm > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (altitude_mm < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)altitude_mm;
}
