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

/*
 * The same, with the sharpness of the IFFT peak (peak over median bin, nrf_cs_ifft_quality.h; NAN = not known).
 *
 * Bench logs of 2026-11-03 (36 successful attempts, every one inside the 5 m cable bound, IFFT peak 26-137 times the median bin):
 * the coarse RTT was missing (0) or 6-7 m away from the PBR answer on the 4 attempts that fell below the 500 production gate, and
 * these distances (4.68, 4.87, 4.95 and 5.01 m) were compatible with the cable bound, not independently proven correct.
 * All attempts of MAIN->G4 were among them, so the pair never got a sample and the whole ranging ended partial. The current
 * heuristic lifts confidence with enough tones and a peak above NRF_CS_IFFT_SHARP_PEAK_RATIO; this does not establish a direct path.
 * A PBR answer more than NRF_CS_SHARP_PEAK_RTT_LIMIT_M from a valid RTT is left to the plain rule. Direction/triangle/cable checks
 * cannot guarantee detection of coherent wrong distances either; see docs/testing/ble6-b5-confidence-diagnostics-2026-11-04.md.
 */
#define NRF_CS_SHARP_PEAK_CONFIDENCE 600U
#define NRF_CS_SHARP_PEAK_RTT_LIMIT_M (3.0f * NRF_CS_RTT_AGREEMENT_M)

uint16_t nrf_cs_measurement_confidence_with_peak(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor
);

/*
 * The factor that decided the confidence of nrf_cs_measurement_confidence_with_peak(), for the log (`why=` of the est line):
 *  NO_ESTIMATE      neither a PBR nor an RTT answer: confidence 0;
 *  BAD_INPUT        a valid answer that is not a number: confidence 0;
 *  TONES_LOW        too few good tones for the phase answer: confidence 0;
 *  SHARP_PEAK       the confidence was lifted to NRF_CS_SHARP_PEAK_CONFIDENCE by a sharp IFFT peak (RTT missing or far off);
 *  RTT_MISSING      the PBR answer stands alone (RTT absent or zero) and is capped at 0.4: below the 500 gate unless the peak is sharp;
 *  RTT_DISAGREES    the PBR answer is farther from the RTT than NRF_CS_RTT_AGREEMENT_M and the confidence decays;
 *  RTT_ONLY         only the coarse RTT answer exists (0.4);
 *  OK               tone coverage (and the RTT where it exists) decided, nothing else.
 */
typedef enum {
    NRF_CS_CONFIDENCE_REASON_UNKNOWN = 0,
    NRF_CS_CONFIDENCE_REASON_OK,
    NRF_CS_CONFIDENCE_REASON_NO_ESTIMATE,
    NRF_CS_CONFIDENCE_REASON_BAD_INPUT,
    NRF_CS_CONFIDENCE_REASON_TONES_LOW,
    NRF_CS_CONFIDENCE_REASON_SHARP_PEAK,
    NRF_CS_CONFIDENCE_REASON_RTT_MISSING,
    NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES,
    NRF_CS_CONFIDENCE_REASON_RTT_ONLY,
} nrf_cs_confidence_reason_t;

nrf_cs_confidence_reason_t nrf_cs_measurement_confidence_reason(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor
);
const char *nrf_cs_confidence_reason_name(nrf_cs_confidence_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif
