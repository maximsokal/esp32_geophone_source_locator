#include "geophone_events.h"

#include <math.h>
#include <stdint.h>


static double window_mean_square(const float *samples, size_t start, size_t end)
{
    double sum = 0.0;
    for (size_t i = start; i < end; ++i) {
        sum += (double)samples[i] * (double)samples[i];
    }
    return end > start ? sum / (double)(end - start) : 0.0;
}


/*
 * AIC onset (Maeda 1985) inside [start, end): the split j minimizing
 *   j ln(var(x[start, start + j))) + (n - j) ln(var(x[start + j, end)))
 * separates the two most different stationary segments -- here the P coda
 * and the S arrival. Returns start + j.
 */
static size_t aic_onset(const float *samples, size_t start, size_t end)
{
    const size_t n = end - start;
    double best = INFINITY;
    size_t best_split = start;
    for (size_t j = 2U; j + 2U <= n; ++j) {
        double sum_left = 0.0, sq_left = 0.0, sum_right = 0.0, sq_right = 0.0;
        for (size_t i = 0U; i < j; ++i) {
            const double x = (double)samples[start + i];
            sum_left += x;
            sq_left += x * x;
        }
        for (size_t i = j; i < n; ++i) {
            const double x = (double)samples[start + i];
            sum_right += x;
            sq_right += x * x;
        }
        const double var_left = sq_left / (double)j - (sum_left / (double)j) * (sum_left / (double)j);
        const double var_right =
            sq_right / (double)(n - j) - (sum_right / (double)(n - j)) * (sum_right / (double)(n - j));
        const double aic =
            (double)j * log(var_left > 1.0e-30 ? var_left : 1.0e-30) +
            (double)(n - j) * log(var_right > 1.0e-30 ? var_right : 1.0e-30);
        if (aic < best) {
            best = aic;
            best_split = start + j;
        }
    }
    return best_split;
}


bool geophone_event_pick_secondary_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    size_t primary_arrival_index,
    const geophone_secondary_pick_config_t *config,
    geophone_channel_pick_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_channel_pick_t) {
        .status = GEOPHONE_PICK_REJECTED_NO_TRIGGER,
        .reason = GEOPHONE_PICK_REASON_NONE,
        .method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED,
        .uncertainty_samples = config != NULL ? config->sta_window_samples : 0u,
    };

    if (
        samples == NULL ||
        config == NULL ||
        config->sta_window_samples == 0u ||
        config->lta_window_samples <= config->sta_window_samples ||
        config->max_separation_samples <= config->min_separation_samples ||
        !isfinite(config->sta_lta_trigger_ratio) ||
        config->sta_lta_trigger_ratio <= 0.0f ||
        primary_arrival_index >= sample_count
    ) {
        return false;
    }

    const size_t sta = config->sta_window_samples;
    const size_t lta = config->lta_window_samples;
    /* The LTA window [onset + sta - lta, onset + sta) must not reach before
     * the P onset, or the P arrival itself would trigger. */
    size_t first = primary_arrival_index + config->min_separation_samples;
    if (primary_arrival_index + (lta - sta) > first) {
        first = primary_arrival_index + (lta - sta);
    }
    size_t last = primary_arrival_index + config->max_separation_samples;
    if (sample_count < sta || last > sample_count - sta) {
        last = sample_count >= sta ? sample_count - sta : 0u;
    }
    if (first > last) {
        return false;
    }
    for (size_t i = first + sta - lta; i < last + sta; ++i) {
        if (!isfinite(samples[i])) {
            return false;
        }
    }

    size_t trigger = SIZE_MAX;
    for (size_t onset = first; onset <= last; ++onset) {
        const double lta_mean = window_mean_square(samples, onset + sta - lta, onset + sta);
        const double sta_mean = window_mean_square(samples, onset, onset + sta);
        if (lta_mean > 0.0 && sta_mean / lta_mean >= (double)config->sta_lta_trigger_ratio) {
            trigger = onset;
            break;
        }
    }
    if (trigger == SIZE_MAX) {
        return false;
    }

    /* AIC over the coda before the trigger and the trigger window. */
    const size_t onset = aic_onset(samples, trigger + sta - lta, trigger + sta);

    const size_t coda_start = onset > primary_arrival_index + (lta - sta) ? onset - (lta - sta) : primary_arrival_index;
    const double coda_rms = sqrt(window_mean_square(samples, coda_start, onset));
    const size_t signal_end = onset + sta <= sample_count ? onset + sta : sample_count;
    const double signal_rms = sqrt(window_mean_square(samples, onset, signal_end));
    const float snr = coda_rms > 0.0 ? (float)(signal_rms / coda_rms) : (signal_rms > 0.0 ? 1.0e6f : 0.0f);

    result->arrival_sample_index = onset;
    result->arrival_subsample_offset = 0.0f;
    result->arrival_time_s = sample_rate_hz > 0u ? (float)((double)onset / (double)sample_rate_hz) : 0.0f;
    result->snr = snr;

    const geophone_pick_quality_config_t quality_config = { .minimum_snr = config->minimum_snr };
    if (onset <= primary_arrival_index || !geophone_event_pick_is_acceptable(snr, &quality_config)) {
        result->status = GEOPHONE_PICK_REJECTED_LOW_SNR;
        return false;
    }

    result->status = GEOPHONE_PICK_ACCEPTED;
    return true;
}
