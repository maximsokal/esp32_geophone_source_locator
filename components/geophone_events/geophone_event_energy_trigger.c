#include "geophone_events.h"

#include <math.h>


static double sum_squares_over(
    const float *samples,
    size_t start,
    size_t count
)
{
    double sum = 0.0;
    for (size_t i = 0u; i < count; ++i) {
        const double value = (double)samples[start + i];
        sum += value * value;
    }
    return sum;
}


bool geophone_event_energy_trigger_detect(
    const float *samples,
    size_t sample_count,
    const geophone_energy_trigger_config_t *config,
    geophone_energy_trigger_result_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_energy_trigger_result_t) { 0 };

    if (
        samples == NULL ||
        config == NULL ||
        config->trigger_window_samples == 0u ||
        config->noise_window_samples == 0u
    ) {
        return false;
    }

    if (sample_count < config->noise_window_samples + config->trigger_window_samples) {
        return false;
    }

    const size_t window = config->trigger_window_samples;

    const float noise_rms =
        (float)sqrt(
            sum_squares_over(samples, 0u, config->noise_window_samples) /
            (double)config->noise_window_samples
        );
    result->noise_rms = noise_rms;

    const size_t last_start = sample_count - window;

    /* Rolling sum of squares over the sliding trigger window, so the whole
     * scan is O(sample_count) instead of O(sample_count * window). */
    double window_sum = sum_squares_over(
        samples,
        config->noise_window_samples,
        window
    );

    for (size_t start = config->noise_window_samples; ; ++start) {
        const float window_rms = (float)sqrt(window_sum / (double)window);

        const bool triggered =
            noise_rms > 0.0f
            ? window_rms >= config->threshold_multiplier * noise_rms
            : window_rms > 0.0f;

        if (triggered) {
            result->found = true;
            result->trigger_index = start;
            result->trigger_rms = window_rms;
            return true;
        }

        if (start >= last_start) {
            break;
        }

        const double outgoing = (double)samples[start] * (double)samples[start];
        const double incoming =
            (double)samples[start + window] * (double)samples[start + window];
        window_sum += incoming - outgoing;
    }

    return false;
}
