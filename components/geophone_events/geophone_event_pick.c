#include "geophone_events.h"

#include <math.h>
#include <stdint.h>


enum
{
    /* The C12 secondary-pick search ends at 320 samples; one sample beyond
     * it keeps known P/S pairs outside the two-independent-events window. */
    PICK_AMBIGUITY_GUARD_MIN_SAMPLES = 321u
};


static size_t saturating_add_size(size_t left, size_t right)
{
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}


bool geophone_event_pick_candidate_after(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    size_t minimum_trigger_index,
    geophone_event_candidate_t *result
)
{
    if (result == NULL) return false;
    *result = (geophone_event_candidate_t) { 0 };
    result->pick.status = GEOPHONE_PICK_REJECTED_NO_TRIGGER;
    if (samples == NULL || config == NULL || sample_rate_hz == 0U ||
        config->noise_window_samples == 0U || config->snr_signal_window_samples == 0U ||
        !isfinite(config->minimum_snr) || config->minimum_snr < 0.0f ||
        !isfinite(config->refine_amplitude_fraction) ||
        config->refine_amplitude_fraction <= 0.0f || config->refine_amplitude_fraction > 1.0f) return false;

    const geophone_sta_lta_config_t detector = {
        .sta_window_samples = config->sta_window_samples,
        .lta_window_samples = config->lta_window_samples,
        .trigger_ratio = config->sta_lta_trigger_ratio
    };
    geophone_sta_lta_episode_t episode;
    if (!geophone_event_sta_lta_episode_after(
            samples, sample_count, &detector, minimum_trigger_index, &episode)) return false;
    result->next_trigger_index = episode.next_trigger_index;
    result->pick.status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
    result->pick.method = GEOPHONE_PICK_METHOD_STA_LTA_REFINED;
    result->pick.uncertainty_samples = saturating_add_size(
        config->refine_lookback_samples, config->refine_lookahead_samples
    );
    if (!episode.complete || episode.trigger_index < config->refine_lookback_samples ||
        config->refine_lookahead_samples >= sample_count - episode.trigger_index) return true;

    const geophone_arrival_refine_config_t refine = {
        .lookback_samples = config->refine_lookback_samples,
        .lookahead_samples = config->refine_lookahead_samples,
        .amplitude_fraction = config->refine_amplitude_fraction
    };
    geophone_arrival_refine_result_t arrival;
    if (!geophone_event_refine_arrival(samples, sample_count, episode.trigger_index, &refine, &arrival)) {
        result->complete = true;
        return true;
    }
    result->pick.arrival_sample_index = arrival.refined_index;
    result->pick.arrival_subsample_offset = arrival.refined_fraction;
    result->pick.arrival_time_s = (float)(
        ((double)arrival.refined_index + (double)arrival.refined_fraction) / (double)sample_rate_hz
    );
    if (arrival.refined_index < config->noise_window_samples ||
        config->snr_signal_window_samples > sample_count - arrival.refined_index) return true;

    const size_t noise_start = arrival.refined_index - config->noise_window_samples;
    result->pick.snr = geophone_event_compute_snr(
        samples + noise_start, sample_count - noise_start, config->noise_window_samples,
        config->noise_window_samples, config->snr_signal_window_samples
    );
    for (size_t i = 0U; i < config->snr_signal_window_samples; ++i) {
        const float magnitude = fabsf(samples[arrival.refined_index + i]);
        if (magnitude > result->peak_abs) result->peak_abs = magnitude;
    }
    const geophone_pick_quality_config_t quality = { .minimum_snr = config->minimum_snr };
    result->pick.status = geophone_event_pick_is_acceptable(result->pick.snr, &quality)
        ? GEOPHONE_PICK_ACCEPTED : GEOPHONE_PICK_REJECTED_LOW_SNR;
    result->complete = true;
    return true;
}


const char *geophone_pick_reason_name(geophone_pick_reason_t reason)
{
    switch (reason) {
    case GEOPHONE_PICK_REASON_NONE: return "none";
    case GEOPHONE_PICK_REASON_SATURATED: return "saturated";
    case GEOPHONE_PICK_REASON_AMBIGUOUS: return "ambiguous";
    }
    return "unknown";
}


bool geophone_event_raw_i16_window_is_saturated(
    const int16_t *raw_samples,
    size_t sample_count
)
{
    if (raw_samples == NULL || sample_count == 0u) {
        return false;
    }

    size_t rail_count = 0u;
    for (size_t i = 0u; i < sample_count; ++i) {
        if (raw_samples[i] == INT16_MIN || raw_samples[i] == INT16_MAX) {
            ++rail_count;
        }
    }

    return geophone_event_i16_rail_count_is_saturated(rail_count, sample_count);
}


bool geophone_event_i16_rail_count_is_saturated(
    size_t rail_count,
    size_t window_samples
)
{
    if (window_samples == 0U) {
        return false;
    }

    const size_t fraction_minimum = window_samples / 200U +
        (window_samples % 200U != 0U ? 1U : 0U);
    const size_t required_rails = fraction_minimum > 8U ? fraction_minimum : 8U;
    return rail_count >= required_rails;
}


static size_t ambiguity_guard_samples(const geophone_channel_pick_config_t *config)
{
    size_t detector_span = saturating_add_size(
        config->sta_window_samples,
        config->lta_window_samples
    );
    detector_span = saturating_add_size(detector_span, 1u);
    return detector_span > (size_t)PICK_AMBIGUITY_GUARD_MIN_SAMPLES
        ? detector_span
        : (size_t)PICK_AMBIGUITY_GUARD_MIN_SAMPLES;
}


static bool has_valid_second_crossing(
    const float *samples,
    size_t sample_count,
    const geophone_channel_pick_config_t *config,
    size_t first_arrival_index
)
{
    const size_t minimum_second_arrival = saturating_add_size(
        first_arrival_index,
        ambiguity_guard_samples(config)
    );
    const geophone_sta_lta_config_t sta_lta_config = {
        .sta_window_samples = config->sta_window_samples,
        .lta_window_samples = config->lta_window_samples,
        .trigger_ratio = config->sta_lta_trigger_ratio
    };
    /* Refinement can move a trigger back by lookback samples. Start the
     * second scan late enough that its refined onset still clears the guard. */
    const size_t minimum_second_trigger = saturating_add_size(
            minimum_second_arrival,
            config->refine_lookback_samples
        );
    geophone_sta_lta_result_t sta_lta_result;
    if (!geophone_event_sta_lta_detect_after(
            samples, sample_count, &sta_lta_config, minimum_second_trigger, &sta_lta_result
        )) {
        return false;
    }

    const geophone_arrival_refine_config_t refine_config = {
        .lookback_samples = config->refine_lookback_samples,
        .lookahead_samples = config->refine_lookahead_samples,
        .amplitude_fraction = config->refine_amplitude_fraction
    };
    geophone_arrival_refine_result_t refine_result;
    if (!geophone_event_refine_arrival(
            samples,
            sample_count,
            sta_lta_result.trigger_index,
            &refine_config,
            &refine_result
        ) || refine_result.refined_index < minimum_second_arrival) {
        return false;
    }

    const float snr = geophone_event_compute_snr(
        samples,
        sample_count,
        config->noise_window_samples,
        refine_result.refined_index,
        config->snr_signal_window_samples
    );
    const geophone_pick_quality_config_t quality_config = {
        .minimum_snr = config->minimum_snr
    };
    return geophone_event_pick_is_acceptable(snr, &quality_config);
}


static bool pick_channel_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    bool raw_i16_saturated,
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
        .arrival_sample_index = 0u,
        .arrival_time_s = 0.0f,
        .snr = 0.0f,
        .uncertainty_samples = config != NULL
            ? saturating_add_size(
                config->refine_lookback_samples,
                config->refine_lookahead_samples
            )
            : 0u
    };

    if (samples == NULL || config == NULL) {
        return false;
    }
    if (raw_i16_saturated) {
        result->status = GEOPHONE_PICK_REJECTED_SATURATED;
        result->reason = GEOPHONE_PICK_REASON_SATURATED;
        return false;
    }

    const geophone_sta_lta_config_t sta_lta_config = {
        .sta_window_samples = config->sta_window_samples,
        .lta_window_samples = config->lta_window_samples,
        .trigger_ratio = config->sta_lta_trigger_ratio
    };
    geophone_sta_lta_result_t sta_lta_result;
    if (!geophone_event_sta_lta_detect(
            samples, sample_count, &sta_lta_config, &sta_lta_result
        )) {
        return false;
    }

    const geophone_arrival_refine_config_t refine_config = {
        .lookback_samples = config->refine_lookback_samples,
        .lookahead_samples = config->refine_lookahead_samples,
        .amplitude_fraction = config->refine_amplitude_fraction
    };
    geophone_arrival_refine_result_t refine_result;
    if (!geophone_event_refine_arrival(
            samples,
            sample_count,
            sta_lta_result.trigger_index,
            &refine_config,
            &refine_result
        )) {
        result->status = GEOPHONE_PICK_REJECTED_NO_REFINEMENT;
        return false;
    }

    const float snr = geophone_event_compute_snr(
        samples,
        sample_count,
        config->noise_window_samples,
        refine_result.refined_index,
        config->snr_signal_window_samples
    );

    result->arrival_sample_index = refine_result.refined_index;
    result->arrival_subsample_offset = refine_result.refined_fraction;
    result->arrival_time_s =
        sample_rate_hz > 0u
        ? (float)(
              ((double)refine_result.refined_index + (double)refine_result.refined_fraction) /
              (double)sample_rate_hz
          )
        : 0.0f;
    result->snr = snr;

    const geophone_pick_quality_config_t quality_config = {
        .minimum_snr = config->minimum_snr
    };
    if (!geophone_event_pick_is_acceptable(snr, &quality_config)) {
        result->status = GEOPHONE_PICK_REJECTED_LOW_SNR;
        return false;
    }

    if (has_valid_second_crossing(
            samples,
            sample_count,
            config,
            refine_result.refined_index
        )) {
        result->status = GEOPHONE_PICK_REJECTED_AMBIGUOUS;
        result->reason = GEOPHONE_PICK_REASON_AMBIGUOUS;
        return false;
    }

    result->status = GEOPHONE_PICK_ACCEPTED;
    return true;
}


bool geophone_event_pick_channel_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    geophone_channel_pick_t *result
)
{
    return pick_channel_arrival(
        samples, sample_count, sample_rate_hz, config, false, result
    );
}


bool geophone_event_pick_channel_arrival_with_saturation(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    bool raw_i16_saturated,
    geophone_channel_pick_t *result
)
{
    return pick_channel_arrival(
        samples, sample_count, sample_rate_hz, config, raw_i16_saturated, result
    );
}
