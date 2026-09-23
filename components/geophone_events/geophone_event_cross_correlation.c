#include "geophone_events.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>


static double correlation_at(
    const float *channel_a,
    const float *channel_b,
    size_t sample_count,
    ptrdiff_t lag
)
{
    const size_t start = lag >= 0 ? 0u : (size_t)(-lag);
    const size_t end =
        lag >= 0 ? sample_count - (size_t)lag : sample_count;

    double sum = 0.0;
    for (size_t i = start; i < end; ++i) {
        const size_t b_index = (size_t)((ptrdiff_t)i + lag);
        sum += (double)channel_a[i] * (double)channel_b[b_index];
    }
    return sum;
}


bool geophone_event_cross_correlate(
    const float *channel_a,
    const float *channel_b,
    size_t sample_count,
    const geophone_cross_correlation_config_t *config,
    geophone_cross_correlation_result_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_cross_correlation_result_t) { 0 };

    if (
        channel_a == NULL ||
        channel_b == NULL ||
        config == NULL ||
        sample_count == 0u ||
        config->max_lag_samples >= sample_count ||
        config->max_lag_samples > (size_t)PTRDIFF_MAX
    ) {
        return false;
    }

    double energy_a = 0.0;
    double energy_b = 0.0;
    for (size_t i = 0u; i < sample_count; ++i) {
        if (!isfinite(channel_a[i]) || !isfinite(channel_b[i])) {
            return false;
        }
        energy_a += (double)channel_a[i] * (double)channel_a[i];
        energy_b += (double)channel_b[i] * (double)channel_b[i];
    }

    if (
        !isfinite(energy_a) ||
        !isfinite(energy_b) ||
        energy_a <= 0.0 ||
        energy_b <= 0.0
    ) {
        return false;
    }

    const double normalizer = sqrt(energy_a * energy_b);
    const ptrdiff_t max_lag = (ptrdiff_t)config->max_lag_samples;

    double best_correlation = -2.0; /* below any achievable normalized value */
    ptrdiff_t best_lag = 0;

    for (ptrdiff_t lag = -max_lag; lag <= max_lag; ++lag) {
        const double correlation = correlation_at(channel_a, channel_b, sample_count, lag) / normalizer;
        if (correlation > best_correlation) {
            best_correlation = correlation;
            best_lag = lag;
        }
    }

    /* MAX-209: parabolic interpolation of the peak for sub-sample delay. */
    double refined = (double)best_lag;
    if (best_lag > -max_lag && best_lag < max_lag) {
        const double left = correlation_at(channel_a, channel_b, sample_count, best_lag - 1) / normalizer;
        const double right = correlation_at(channel_a, channel_b, sample_count, best_lag + 1) / normalizer;
        const double denominator = left - 2.0 * best_correlation + right;
        if (denominator < 0.0) {
            const double shift = 0.5 * (left - right) / denominator;
            if (shift > -1.0 && shift < 1.0) {
                refined += shift;
            }
        }
    }

    result->found = true;
    result->delay_samples = (int32_t)best_lag;
    result->delay_refined_samples = refined;
    result->peak_correlation = (float)best_correlation;
    return true;
}
