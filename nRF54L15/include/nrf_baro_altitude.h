#ifndef NRF_BARO_ALTITUDE_H
#define NRF_BARO_ALTITUDE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The linear model below: 12 Pa per metre (rho g of the ISA sea-level atmosphere at 15 C). */
#define NRF_BARO_LINEAR_PA_PER_M 12.0
/* Specific gas constant of dry air (R / M_d, CODATA R, M_d = 28.9649 g/mol) and standard gravity (ISO 80000-3). */
#define NRF_BARO_DRY_AIR_GAS_CONSTANT 287.05
#define NRF_BARO_STANDARD_GRAVITY 9.80665

/*
 * Keep the REMOTE's linear approximation shared with MAIN: relative Z is
 * rel_i - rel_MAIN, so matching conversion matters more than absolute
 * accuracy -- the drift common to both cancels in the difference. The
 * difference itself is a pressure difference divided by 12 Pa/m, while the
 * real vertical gradient is rho g with rho = p / (R_d T) (hydrostatic
 * equation with the ideal gas law; the hypsometric equation over a few
 * metres). 12 Pa/m holds only near 101 kPa and 15 C: a 2 m difference reads
 * +8 % at -10 C / 100 kPa and -8.5 % at 35 C / 99 kPa. Scale it with
 * nrf_baro_density_corrected_height_mm() before using it as a height.
 */
int32_t nrf_baro_pressure_to_altitude_mm(uint32_t pressure_pa);
/* The same model for a pressure in hundredths of a pascal (the sensors resolve 0.01 Pa). */
int32_t nrf_baro_pressure_x100_to_altitude_mm(uint32_t pressure_x100_pa);

/*
 * A height difference of the linear model (a pressure difference / 12 Pa/m) turned into metres of the air actually
 * there: times 12 / (rho g) = 12 R_d T / (p g), with p and T measured by the same barometer at the site (MAIN). Dry air:
 * humidity raises the virtual temperature by under 1 % below 30 C. Without a usable p/T (env_valid false, or outside
 * 30..110 kPa / -40..85 C, the BMP280 range) the linear value is returned unchanged.
 */
static inline int32_t nrf_baro_density_corrected_height_mm(
    int64_t linear_height_mm,
    bool env_valid,
    uint32_t pressure_pa,
    int32_t temperature_mc)
{
    double height_mm = (double)linear_height_mm;
    if (env_valid && pressure_pa >= 30000U && pressure_pa <= 110000U &&
        temperature_mc >= -40000 && temperature_mc <= 85000) {
        const double temperature_k = (double)temperature_mc / 1000.0 + 273.15;
        height_mm *= NRF_BARO_LINEAR_PA_PER_M * NRF_BARO_DRY_AIR_GAS_CONSTANT * temperature_k /
            ((double)pressure_pa * NRF_BARO_STANDARD_GRAVITY);
    }
    if (height_mm >= (double)INT32_MAX) return INT32_MAX;
    if (height_mm <= (double)INT32_MIN) return INT32_MIN;
    return (int32_t)(height_mm >= 0.0 ? height_mm + 0.5 : height_mm - 0.5);
}

#ifdef __cplusplus
}
#endif

#endif
