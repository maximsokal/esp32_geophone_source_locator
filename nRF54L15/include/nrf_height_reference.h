#ifndef NRF_HEIGHT_REFERENCE_H
#define NRF_HEIGHT_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf_upstream_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t altitude_mm;
    uint16_t generation;
    nrf_upstream_reference_state_t state;
} nrf_height_reference_t;

void nrf_height_reference_init(nrf_height_reference_t *reference);

bool nrf_height_reference_apply(
    nrf_height_reference_t *reference,
    uint8_t action,
    uint16_t generation,
    bool imu_valid,
    bool env_valid,
    bool stationary,
    int32_t filtered_altitude_mm);

int32_t nrf_height_reference_relative_z_mm(
    const nrf_height_reference_t *reference,
    bool current_altitude_valid,
    int32_t filtered_altitude_mm);

#ifdef __cplusplus
}
#endif

#endif
