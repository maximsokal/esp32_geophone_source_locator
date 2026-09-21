#include "geophone_events.h"

#include <math.h>


float geophone_event_compute_snr(
    const float *samples,
    size_t sample_count,
    size_t noise_window_samples,
    size_t arrival_index,
    size_t signal_window_samples
)
{
    if (
        samples == NULL ||
        noise_window_samples == 0u ||
        signal_window_samples == 0u ||
        noise_window_samples > sample_count ||
        arrival_index >= sample_count
    ) {
        return 0.0f;
    }

    for (size_t i = 0u; i < sample_count; ++i) {
        if (!isfinite(samples[i])) {
            return 0.0f;
        }
    }

    double noise_sum_squares = 0.0;
    for (size_t i = 0u; i < noise_window_samples; ++i) {
        noise_sum_squares += (double)samples[i] * (double)samples[i];
    }
    const double noise_rms = sqrt(noise_sum_squares / (double)noise_window_samples);

    const size_t samples_after_arrival = sample_count - arrival_index;
    const size_t signal_end =
        signal_window_samples >= samples_after_arrival
        ? sample_count
        : arrival_index + signal_window_samples;

    float signal_peak = 0.0f;
    for (size_t i = arrival_index; i < signal_end; ++i) {
        const float magnitude = fabsf(samples[i]);
        if (magnitude > signal_peak) {
            signal_peak = magnitude;
        }
    }

    if (noise_rms <= 0.0) {
        /* No measurable background at all: report a large but finite value
         * rather than dividing by zero, so a finite minimum_snr threshold
         * still compares meaningfully instead of ending up NaN. */
        return signal_peak > 0.0f ? (float)1.0e6 : 0.0f;
    }

    return (float)((double)signal_peak / noise_rms);
}


bool geophone_event_pick_is_acceptable(
    float snr,
    const geophone_pick_quality_config_t *config
)
{
    if (config == NULL) {
        return false;
    }

    return
        isfinite(snr) &&
        isfinite(config->minimum_snr) &&
        config->minimum_snr >= 0.0f &&
        snr >= config->minimum_snr;
}
