#ifndef NRF_CS_MEASUREMENT_QUALITY_H
#define NRF_CS_MEASUREMENT_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MAX-206: per-procedure Channel Sounding confidence in
 * [0, NRF_CS_CONFIDENCE_MAX] from the evidence the distance estimator
 * actually saw, instead of a constant 1000 for every success.
 *
 * - Tone coverage: below `tone_ok_threshold` HIGH-quality tone channels the
 *   phase-based estimate is not trusted (0). At the threshold the factor is
 *   0.6, rising linearly to 1.0 at three times the threshold -- more
 *   channels constrain the phase slope and suppress multipath aliases.
 * - PBR/RTT agreement: when both estimators produced a value, their
 *   difference beyond NRF_CS_RTT_AGREEMENT_M (the coarse AA-only RTT's
 *   own error scale) decays the confidence as a Gaussian with that same
 *   scale; a phase-based answer many metres from the time-of-flight answer
 *   can indicate a wrapped/multipath solution. Without RTT the PBR score is
 *   limited to the same 0.4 ceiling as a coarse RTT-only result: tone coverage
 *   alone does not establish absolute accuracy. At the production threshold of
 *   500 these results remain logged but are rejected for automatic geometry.
 *
 * The scale constants are a documented starting point to be calibrated by
 * the MAX-143 HIL study, not measured values.
 */
#define NRF_CS_RTT_AGREEMENT_M 2.5f

uint16_t nrf_cs_measurement_confidence(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m
);

#ifdef __cplusplus
}
#endif

#endif
