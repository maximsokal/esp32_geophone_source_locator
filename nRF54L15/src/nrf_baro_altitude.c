#include "nrf_baro_altitude.h"

#include <limits.h>

#define NRF_BARO_SEA_LEVEL_PRESSURE_PA 101325
#define NRF_BARO_ALTITUDE_PA_PER_METER 12

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
