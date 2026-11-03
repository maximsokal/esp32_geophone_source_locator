#include "nrf_cs_measurement_quality.h"

#include <math.h>

#include "nrf_cs_ifft_quality.h"
#include "nrf_cs_scheduler.h"

#define NRF_CS_SINGLE_METHOD_CONFIDENCE_FACTOR 0.4f


uint16_t nrf_cs_measurement_confidence(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m
)
{
    float factor;

    if ((pbr_valid && !isfinite(best_m)) || (rtt_valid && !isfinite(rtt_m))) {
        return 0U;
    }

    if (pbr_valid) {
        if (tone_ok_threshold == 0U || good_tone_channels < tone_ok_threshold) {
            return 0U;
        }
        const float extra =
            (float)(good_tone_channels - tone_ok_threshold) / (2.0f * (float)tone_ok_threshold);
        factor = 0.6f + 0.4f * (extra > 1.0f ? 1.0f : extra);

        if (!rtt_valid) {
            /* Tone count alone cannot distinguish a reflected path from the true distance. */
            factor *= NRF_CS_SINGLE_METHOD_CONFIDENCE_FACTOR;
        } else {
            const float excess = fabsf(best_m - rtt_m) - NRF_CS_RTT_AGREEMENT_M;
            if (excess > 0.0f) {
                const float ratio = excess / NRF_CS_RTT_AGREEMENT_M;
                factor *= expf(-0.5f * ratio * ratio);
            }
        }
    } else if (rtt_valid) {
        factor = NRF_CS_SINGLE_METHOD_CONFIDENCE_FACTOR;
    } else {
        return 0U;
    }

    const float confidence = factor * (float)NRF_CS_CONFIDENCE_MAX + 0.5f;
    if (confidence <= 0.0f) {
        return 0U;
    }
    if (confidence >= (float)NRF_CS_CONFIDENCE_MAX) {
        return NRF_CS_CONFIDENCE_MAX;
    }
    return (uint16_t)confidence;
}

uint16_t nrf_cs_measurement_confidence_with_peak(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor
)
{
    const uint16_t plain = nrf_cs_measurement_confidence(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m);

    if (!pbr_valid || !isfinite(best_m) || tone_ok_threshold == 0U || good_tone_channels < tone_ok_threshold ||
        !isfinite(peak_to_floor) || peak_to_floor <= NRF_CS_IFFT_SHARP_PEAK_RATIO) {
        return plain;
    }
    if (rtt_valid && (!isfinite(rtt_m) || fabsf(best_m - rtt_m) > NRF_CS_SHARP_PEAK_RTT_LIMIT_M)) {
        return plain;
    }
    return plain > NRF_CS_SHARP_PEAK_CONFIDENCE ? plain : NRF_CS_SHARP_PEAK_CONFIDENCE;
}

nrf_cs_confidence_reason_t nrf_cs_measurement_confidence_reason(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor
)
{
    if ((pbr_valid && !isfinite(best_m)) || (rtt_valid && !isfinite(rtt_m))) {
        return NRF_CS_CONFIDENCE_REASON_BAD_INPUT;
    }
    if (!pbr_valid && !rtt_valid) {
        return NRF_CS_CONFIDENCE_REASON_NO_ESTIMATE;
    }
    if (!pbr_valid) {
        return NRF_CS_CONFIDENCE_REASON_RTT_ONLY;
    }
    if (tone_ok_threshold == 0U || good_tone_channels < tone_ok_threshold) {
        return NRF_CS_CONFIDENCE_REASON_TONES_LOW;
    }
    const uint16_t plain = nrf_cs_measurement_confidence(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m);
    const uint16_t lifted = nrf_cs_measurement_confidence_with_peak(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor);
    if (lifted > plain) {
        return NRF_CS_CONFIDENCE_REASON_SHARP_PEAK;
    }
    if (!rtt_valid) {
        return NRF_CS_CONFIDENCE_REASON_RTT_MISSING;
    }
    if (fabsf(best_m - rtt_m) > NRF_CS_RTT_AGREEMENT_M) {
        return NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES;
    }
    return NRF_CS_CONFIDENCE_REASON_OK;
}

const char *nrf_cs_confidence_reason_name(nrf_cs_confidence_reason_t reason)
{
    switch (reason) {
        case NRF_CS_CONFIDENCE_REASON_OK: return "ok";
        case NRF_CS_CONFIDENCE_REASON_NO_ESTIMATE: return "no_estimate";
        case NRF_CS_CONFIDENCE_REASON_BAD_INPUT: return "bad_input";
        case NRF_CS_CONFIDENCE_REASON_TONES_LOW: return "tones_low";
        case NRF_CS_CONFIDENCE_REASON_SHARP_PEAK: return "sharp_peak";
        case NRF_CS_CONFIDENCE_REASON_RTT_MISSING: return "rtt_missing";
        case NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES: return "rtt_disagrees";
        case NRF_CS_CONFIDENCE_REASON_RTT_ONLY: return "rtt_only";
        case NRF_CS_CONFIDENCE_REASON_UNKNOWN:
        default: return "unknown";
    }
}
