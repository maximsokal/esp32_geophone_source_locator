#include "geophone_events.h"

#include <math.h>


bool geophone_event_refine_arrival(
    const float *samples,
    size_t sample_count,
    size_t coarse_trigger_index,
    const geophone_arrival_refine_config_t *config,
    geophone_arrival_refine_result_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_arrival_refine_result_t) { 0 };

    if (
        samples == NULL ||
        config == NULL ||
        config->amplitude_fraction <= 0.0f ||
        sample_count == 0u ||
        coarse_trigger_index >= sample_count
    ) {
        return false;
    }

    const size_t window_start =
        config->lookback_samples > coarse_trigger_index
        ? 0u
        : coarse_trigger_index - config->lookback_samples;

    const size_t window_end_exclusive =
        coarse_trigger_index + config->lookahead_samples + 1u > sample_count
        ? sample_count
        : coarse_trigger_index + config->lookahead_samples + 1u;

    float local_peak = 0.0f;
    for (size_t i = window_start; i < window_end_exclusive; ++i) {
        const float magnitude = fabsf(samples[i]);
        if (magnitude > local_peak) {
            local_peak = magnitude;
        }
    }

    if (local_peak <= 0.0f) {
        return false;
    }

    result->local_peak_abs = local_peak;

    const float threshold = config->amplitude_fraction * local_peak;
    for (size_t i = window_start; i < window_end_exclusive; ++i) {
        if (fabsf(samples[i]) >= threshold) {
            result->found = true;
            result->refined_index = i;
            return true;
        }
    }

    return false;
}
