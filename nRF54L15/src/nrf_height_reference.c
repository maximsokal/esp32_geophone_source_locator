#include "nrf_height_reference.h"

#include <limits.h>
#include <stddef.h>

void nrf_height_reference_init(nrf_height_reference_t *reference)
{
    if (reference == NULL) {
        return;
    }
    *reference = (nrf_height_reference_t){
        .state = NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED,
    };
}

bool nrf_height_reference_apply(
    nrf_height_reference_t *reference,
    uint8_t action,
    uint16_t generation,
    bool imu_valid,
    bool env_valid,
    bool stationary,
    int32_t filtered_altitude_mm)
{
    if (reference == NULL) {
        return false;
    }
    if (action == NRF_UPSTREAM_HEIGHT_REFERENCE_CLEAR) {
        reference->altitude_mm = 0;
        reference->generation = generation;
        reference->state = NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED;
        return true;
    }
    if (action != NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE) {
        return false;
    }

    reference->altitude_mm = 0;
    if (!imu_valid || !env_valid) {
        reference->state = NRF_UPSTREAM_REFERENCE_SENSOR_INVALID;
    } else if (!stationary) {
        reference->state = NRF_UPSTREAM_REFERENCE_REJECTED_MOVING;
    } else {
        reference->altitude_mm = filtered_altitude_mm;
        reference->generation = generation;
        reference->state = NRF_UPSTREAM_REFERENCE_VALID;
    }
    return true;
}

int32_t nrf_height_reference_relative_z_mm(
    const nrf_height_reference_t *reference,
    bool current_altitude_valid,
    int32_t filtered_altitude_mm)
{
    int64_t relative;
    if (reference == NULL || !current_altitude_valid ||
        reference->state != NRF_UPSTREAM_REFERENCE_VALID) {
        return 0;
    }
    relative = (int64_t)filtered_altitude_mm - reference->altitude_mm;
    if (relative > INT32_MAX) {
        return INT32_MAX;
    }
    if (relative < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)relative;
}
