#include "geophone_events.h"

#include <math.h>


static bool samples_are_finite(
    const float *samples,
    size_t sample_count
)
{
    for (size_t i = 0u; i < sample_count; ++i) {
        if (!isfinite(samples[i])) {
            return false;
        }
    }
    return true;
}


bool geophone_event_sta_lta_detect(
    const float *samples,
    size_t sample_count,
    const geophone_sta_lta_config_t *config,
    geophone_sta_lta_result_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_sta_lta_result_t) { 0 };

    if (
        samples == NULL ||
        config == NULL ||
        config->sta_window_samples == 0u ||
        config->lta_window_samples <= config->sta_window_samples ||
        !isfinite(config->trigger_ratio) ||
        config->trigger_ratio <= 0.0f
    ) {
        return false;
    }

    const size_t sta_w = config->sta_window_samples;
    const size_t lta_w = config->lta_window_samples;

    if (sample_count < lta_w || !samples_are_finite(samples, sample_count)) {
        return false;
    }

    /* Rolling sums of squares for the trailing LTA window and its trailing
     * sub-window (the STA window), both ending at the same instant. Sliding
     * one sample forward updates each in O(1). */
    double lta_sum = 0.0;
    for (size_t k = 0u; k < lta_w; ++k) {
        lta_sum += (double)samples[k] * (double)samples[k];
    }

    double sta_sum = 0.0;
    for (size_t k = lta_w - sta_w; k < lta_w; ++k) {
        sta_sum += (double)samples[k] * (double)samples[k];
    }

    for (size_t end = lta_w; ; ++end) {
        const double lta_mean = lta_sum / (double)lta_w;

        if (lta_mean > 0.0) {
            const double sta_mean = sta_sum / (double)sta_w;
            const double ratio = sta_mean / lta_mean;

            if (ratio >= (double)config->trigger_ratio) {
                result->found = true;
                result->trigger_index = end - sta_w;
                result->ratio_at_trigger = (float)ratio;
                return true;
            }
        }

        if (end >= sample_count) {
            break;
        }

        const double incoming = (double)samples[end] * (double)samples[end];
        lta_sum += incoming - (double)samples[end - lta_w] * (double)samples[end - lta_w];
        sta_sum += incoming - (double)samples[end - sta_w] * (double)samples[end - sta_w];
    }

    return false;
}
