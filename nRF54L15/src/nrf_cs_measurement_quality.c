#include "nrf_cs_measurement_quality.h"

#include <math.h>

#include "nrf_cs_iq_gap_fill.h"
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

uint16_t nrf_cs_measurement_confidence_with_coverage(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap
)
{
    if (longest_gap >= NRF_CS_IQ_GAP_FILL_LIMIT) {
        return nrf_cs_measurement_confidence(
            good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m);
    }
    return nrf_cs_measurement_confidence_with_peak(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor);
}

nrf_cs_confidence_reason_t nrf_cs_measurement_confidence_reason_with_coverage(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap
)
{
    const nrf_cs_confidence_reason_t reason = nrf_cs_measurement_confidence_reason(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor);
    if (longest_gap >= NRF_CS_IQ_GAP_FILL_LIMIT && reason == NRF_CS_CONFIDENCE_REASON_SHARP_PEAK) {
        return NRF_CS_CONFIDENCE_REASON_WIDE_GAP;
    }
    return reason;
}

static bool uncorroborated_peak_cap_applies(
    float best_m,
    float ifft_m,
    float phase_slope_m,
    bool ifft_on_bin_grid
)
{
    if (!isfinite(ifft_m) || best_m != ifft_m) {
        return false;
    }
    return ifft_on_bin_grid ||
        (isfinite(phase_slope_m) && fabsf(ifft_m - phase_slope_m) > NRF_CS_METHOD_AGREEMENT_M);
}

uint16_t nrf_cs_measurement_confidence_with_methods(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap,
    float ifft_m,
    float phase_slope_m,
    bool ifft_on_bin_grid
)
{
    const uint16_t base = nrf_cs_measurement_confidence_with_coverage(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor, longest_gap);
    if (uncorroborated_peak_cap_applies(best_m, ifft_m, phase_slope_m, ifft_on_bin_grid) &&
        base > NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE) {
        return NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE;
    }
    return base;
}

nrf_cs_confidence_reason_t nrf_cs_measurement_confidence_reason_with_methods(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap,
    float ifft_m,
    float phase_slope_m,
    bool ifft_on_bin_grid
)
{
    const uint16_t base = nrf_cs_measurement_confidence_with_coverage(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor, longest_gap);
    if (uncorroborated_peak_cap_applies(best_m, ifft_m, phase_slope_m, ifft_on_bin_grid) &&
        base > NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE) {
        return ifft_on_bin_grid ? NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED
                                : NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE;
    }
    return nrf_cs_measurement_confidence_reason_with_coverage(
        good_tone_channels, tone_ok_threshold, pbr_valid, rtt_valid, best_m, rtt_m, peak_to_floor, longest_gap);
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
        case NRF_CS_CONFIDENCE_REASON_WIDE_GAP: return "wide_gap";
        case NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED: return "peak_compensated";
        case NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE: return "methods_disagree";
        case NRF_CS_CONFIDENCE_REASON_UNKNOWN:
        default: return "unknown";
    }
}
