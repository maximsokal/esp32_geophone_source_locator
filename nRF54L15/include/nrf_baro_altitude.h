#ifndef NRF_BARO_ALTITUDE_H
#define NRF_BARO_ALTITUDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Keep the REMOTE's linear approximation shared with MAIN: relative Z is
 * rel_i - rel_MAIN, so matching conversion matters more than absolute
 * accuracy. Over a few metres its error from the standard barometric formula
 * is under about 1.5%; revisit only if HIL measurements justify it.
 */
int32_t nrf_baro_pressure_to_altitude_mm(uint32_t pressure_pa);

#ifdef __cplusplus
}
#endif

#endif
