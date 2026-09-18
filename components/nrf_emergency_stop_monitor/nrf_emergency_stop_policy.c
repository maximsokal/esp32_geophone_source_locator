#include "nrf_emergency_stop_policy.h"

#include <stdlib.h>


static int32_t iabs32(int32_t value)
{
    return value < 0 ? -value : value;
}


bool nrf_emergency_stop_channel_triggered(
    const nrf_emergency_stop_channel_sample_t *baseline,
    const nrf_emergency_stop_channel_sample_t *current
)
{
    if (baseline == NULL || current == NULL || !baseline->valid || !current->valid) {
        return false;
    }

    if (iabs32(current->altitude_mm - baseline->altitude_mm) > NRF_EMERGENCY_STOP_ALTITUDE_THRESHOLD_MM) {
        return true;
    }

    for (size_t axis = 0U; axis < 3U; ++axis) {
        if (iabs32(current->accel_mg[axis] - baseline->accel_mg[axis]) <= NRF_EMERGENCY_STOP_ACCEL_THRESHOLD_MG) {
            return false;
        }
    }

    return true;
}


bool nrf_emergency_stop_any_channel_triggered(
    const nrf_emergency_stop_channel_sample_t *baseline,
    const nrf_emergency_stop_channel_sample_t *current,
    size_t count,
    size_t *triggered_channel
)
{
    if (baseline == NULL || current == NULL) {
        return false;
    }

    for (size_t channel = 0U; channel < count; ++channel) {
        if (nrf_emergency_stop_channel_triggered(&baseline[channel], &current[channel])) {
            if (triggered_channel != NULL) {
                *triggered_channel = channel;
            }
            return true;
        }
    }

    return false;
}
