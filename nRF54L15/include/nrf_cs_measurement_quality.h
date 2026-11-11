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
    NRF_CS_CONFIDENCE_REASON_WIDE_GAP,
    NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED,
    NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE,
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

/*
 * The same two functions, aware of the IFFT input coverage. The IFFT runs over 75 slots and slots without a usable tone are zeros;
 * nrf_cs_iq_gap_fill.h fills only holes shorter than NRF_CS_IQ_GAP_FILL_LIMIT (30 slots, a gap-fill limit, not a measured value).
 * A run of `longest_gap` >= that limit stays zeros and can make a side lobe that looks like a sharp peak at a wrong distance, so
 * the sharp-peak lift is not applied: the result keeps the plain confidence. The reason is WIDE_GAP exactly when the gap removed
 * a lift (the reason would otherwise have been SHARP_PEAK); in every other case it is the reason of the function without coverage.
 */
uint16_t nrf_cs_measurement_confidence_with_coverage(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap
);

nrf_cs_confidence_reason_t nrf_cs_measurement_confidence_reason_with_coverage(
    uint16_t good_tone_channels,
    uint16_t tone_ok_threshold,
    bool pbr_valid,
    bool rtt_valid,
    float best_m,
    float rtt_m,
    float peak_to_floor,
    uint8_t longest_gap
);

/*
 * The same, with two signs that the IFFT answer is not reliable (bench logs of 2026-11-05 and 2026-11-10, 48 measurements at known positions):
 * every IFFT distance that sat exactly on the IFFT bin grid (the library skipped its peak interpolation) belonged to a pair whose
 * readings jumped by 0.9-2.9 m between attempts, and unstable pairs had IFFT and phase-slope distances up to 3.7 m apart while stable
 * pairs agreed within 0.10-0.77 m. When `best_m` is the IFFT answer and either sign is present, the confidence is capped at
 * NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE, below the 500 production gate; the sample is still logged. Both numbers come from these
 * 48 bench measurements, not from a calibrated study. `ifft_on_bin_grid` is computed by the caller (nrf_cs_ifft_on_bin_grid()), so
 * this module does not need the NFFT size; a NAN `phase_slope_m` is "unknown" and never counts as disagreement.
 * The reason is PEAK_COMPENSATED (on grid, wins when both signs are present) or METHODS_DISAGREE exactly when the cap lowered the
 * confidence; in every other case it is the reason of the function with coverage.
 */
#define NRF_CS_METHOD_AGREEMENT_M 1.0f
#define NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE 400U

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
);

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
);

#ifdef __cplusplus
}
#endif

#endif
