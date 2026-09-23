#include "geophone_events.h"

#include <stdint.h>


static size_t saturating_add_size(size_t left, size_t right)
{
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}


bool geophone_event_pick_channel_arrival(
    const float *samples,
    size_t sample_count,
    uint32_t sample_rate_hz,
    const geophone_channel_pick_config_t *config,
    geophone_channel_pick_t *result
)
{
    if (result == NULL) {
        return false;
    }

    *result = (geophone_channel_pick_t) {
        .status = GEOPHONE_PICK_REJECTED_NO_TRIGGER,
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

    result->status = GEOPHONE_PICK_ACCEPTED;
    return true;
}
