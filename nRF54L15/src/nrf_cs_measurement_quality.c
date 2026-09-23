#include "nrf_cs_measurement_quality.h"

#include <math.h>

#include "nrf_cs_scheduler.h"


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

    if (pbr_valid) {
        if (tone_ok_threshold == 0U || good_tone_channels < tone_ok_threshold) {
            return 0U;
        }
        const float extra =
            (float)(good_tone_channels - tone_ok_threshold) / (2.0f * (float)tone_ok_threshold);
        factor = 0.6f + 0.4f * (extra > 1.0f ? 1.0f : extra);

        if (rtt_valid) {
            if (!isfinite(best_m) || !isfinite(rtt_m)) {
                return 0U;
            }
            const float excess = fabsf(best_m - rtt_m) - NRF_CS_RTT_AGREEMENT_M;
            if (excess > 0.0f) {
                const float ratio = excess / NRF_CS_RTT_AGREEMENT_M;
                factor *= expf(-0.5f * ratio * ratio);
            }
        }
    } else if (rtt_valid) {
        factor = 0.4f;
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
