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
        !isfinite(config->amplitude_fraction) ||
        config->amplitude_fraction <= 0.0f ||
        config->amplitude_fraction > 1.0f ||
        sample_count == 0u ||
        coarse_trigger_index >= sample_count
    ) {
        return false;
    }

    for (size_t i = 0u; i < sample_count; ++i) {
        if (!isfinite(samples[i])) {
            return false;
        }
    }

    const size_t window_start =
        config->lookback_samples > coarse_trigger_index
        ? 0u
        : coarse_trigger_index - config->lookback_samples;

    const size_t samples_after_coarse = sample_count - coarse_trigger_index;
    const size_t window_end_exclusive =
        config->lookahead_samples >= samples_after_coarse
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

    size_t peak_index = window_start;
    for (size_t i = window_start; i < window_end_exclusive; ++i) {
        if (fabsf(samples[i]) == local_peak) {
            peak_index = i;
            break;
        }
    }

    /*
     * MAX-209: the onset is the first threshold crossing that belongs to the
     * peak's own burst: the mean |x| from the candidate to the peak must be
     * at least half the threshold. A lone early noise spike above the
     * threshold is followed by a quiet gap, which pulls that mean far below
     * it, so the spike is skipped instead of dragging the pick early. The
     * peak itself always qualifies, so a crossing is always found.
     */
    const float threshold = config->amplitude_fraction * local_peak;
    for (size_t i = window_start; i <= peak_index; ++i) {
        if (fabsf(samples[i]) < threshold) {
            continue;
        }

        double sum_abs = 0.0;
        for (size_t k = i; k <= peak_index; ++k) {
            sum_abs += (double)fabsf(samples[k]);
        }
        const double mean_abs = sum_abs / (double)(peak_index - i + 1U);
        if (mean_abs < 0.5 * (double)threshold) {
            continue;
        }

        result->found = true;
        result->refined_index = i;
        result->refined_fraction = 0.0f;
        if (i > window_start) {
            const float before = fabsf(samples[i - 1U]);
            const float at = fabsf(samples[i]);
            if (before < threshold && at > before) {
                /* Linear interpolation of the crossing between i-1 and i. */
                result->refined_fraction = (threshold - before) / (at - before) - 1.0f;
            }
        }
        return true;
    }

    return false;
}
