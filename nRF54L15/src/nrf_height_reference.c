#include "nrf_height_reference.h"

#include <limits.h>
#include <stddef.h>

int32_t nrf_height_reference_window_centre_mm(
    const int32_t *altitudes_mm,
    size_t count,
    int32_t fallback_mm)
{
    if (altitudes_mm == NULL || count < 3U) {
        return fallback_mm;
    }
    if (count > NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES) {
        altitudes_mm += count - NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES;
        count = NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES;
    }
    double sorted[NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES];
    double deviation[NRF_HEIGHT_REFERENCE_WINDOW_SAMPLES];
    for (size_t i = 0U; i < count; ++i) {
        sorted[i] = (double)altitudes_mm[i];
    }
    for (size_t i = 1U; i < count; ++i) {
        const double key = sorted[i];
        size_t j = i;
        while (j > 0U && sorted[j - 1U] > key) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = key;
    }
    const double median = (count % 2U) == 1U
        ? sorted[count / 2U] : 0.5 * (sorted[count / 2U - 1U] + sorted[count / 2U]);
    for (size_t i = 0U; i < count; ++i) {
        const double d = (double)altitudes_mm[i] - median;
        deviation[i] = d < 0.0 ? -d : d;
    }
    for (size_t i = 1U; i < count; ++i) {
        const double key = deviation[i];
        size_t j = i;
        while (j > 0U && deviation[j - 1U] > key) {
            deviation[j] = deviation[j - 1U];
            --j;
        }
        deviation[j] = key;
    }
    const double mad = (count % 2U) == 1U
        ? deviation[count / 2U] : 0.5 * (deviation[count / 2U - 1U] + deviation[count / 2U]);
    double sigma_r = 1.4826 * mad;
    if (sigma_r < 40.0) {
        sigma_r = 40.0;
    }
    size_t outliers = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const double d = (double)altitudes_mm[i] - median;
        if ((d < 0.0 ? -d : d) > 3.0 * sigma_r) {
            ++outliers;
        }
    }
    const size_t allowed = count / 8U > 1U ? count / 8U : 1U;
    const bool drop = outliers <= allowed;
    double sum = 0.0;
    size_t used = 0U;
    for (size_t i = 0U; i < count; ++i) {
        const double d = (double)altitudes_mm[i] - median;
        if (drop && (d < 0.0 ? -d : d) > 3.0 * sigma_r) {
            continue;
        }
        sum += (double)altitudes_mm[i];
        ++used;
    }
    const double centre = sum / (double)used;
    return (int32_t)(centre >= 0.0 ? centre + 0.5 : centre - 0.5);
}

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
        reference->has_last_good = false;
        reference->last_relative_z_mm = 0;
        reference->altitude_mm = 0;
        reference->generation = generation;
        reference->state = NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED;
        return true;
    }
    if (action != NRF_UPSTREAM_HEIGHT_REFERENCE_CAPTURE) {
        return false;
    }

    reference->altitude_mm = 0;
    reference->has_last_good = false;
    reference->last_relative_z_mm = 0;
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

nrf_upstream_reference_state_t nrf_height_reference_sample(
    nrf_height_reference_t *reference,
    bool current_altitude_valid,
    int32_t filtered_altitude_mm,
    uint64_t now_ms,
    int32_t *relative_z_mm)
{
    if (relative_z_mm != NULL) {
        *relative_z_mm = 0;
    }
    if (reference == NULL || relative_z_mm == NULL) {
        return NRF_UPSTREAM_REFERENCE_NOT_ESTABLISHED;
    }
    if (reference->state != NRF_UPSTREAM_REFERENCE_VALID) {
        return reference->state;
    }
    if (current_altitude_valid) {
        *relative_z_mm = nrf_height_reference_relative_z_mm(
            reference, true, filtered_altitude_mm);
        reference->last_relative_z_mm = *relative_z_mm;
        reference->last_good_uptime_ms = now_ms;
        reference->has_last_good = true;
        return NRF_UPSTREAM_REFERENCE_VALID;
    }
    if (!reference->has_last_good) {
        /* Captured at Z = 0; the hold clock starts at the first sample without a barometer. */
        reference->last_relative_z_mm = 0;
        reference->last_good_uptime_ms = now_ms;
        reference->has_last_good = true;
    }
    if (now_ms >= reference->last_good_uptime_ms &&
        now_ms - reference->last_good_uptime_ms <= NRF_HEIGHT_REFERENCE_HOLD_MS) {
        *relative_z_mm = reference->last_relative_z_mm;
        return NRF_UPSTREAM_REFERENCE_VALID;
    }
    return NRF_UPSTREAM_REFERENCE_SENSOR_INVALID;
}
