#include <assert.h>
#include <math.h>
#include <string.h>

#include "nrf_cs_ifft_quality.h"
#include "nrf_cs_iq_gap_fill.h"
#include "nrf_cs_measurement_quality.h"
#include "nrf_cs_scheduler.h"


static void test_tone_coverage_scales_confidence(void)
{
    /* Below threshold: the phase estimate is not trusted. */
    assert(nrf_cs_measurement_confidence(14U, 15U, true, false, 3.0f, NAN) == 0U);
    /* Without RTT the number of tones cannot certify absolute distance accuracy. */
    assert(nrf_cs_measurement_confidence(15U, 15U, true, false, 3.0f, NAN) == 240U);
    assert(nrf_cs_measurement_confidence(45U, 15U, true, false, 3.0f, NAN) == 400U);
    assert(nrf_cs_measurement_confidence(70U, 15U, true, false, 3.0f, NAN) == 400U);
    /* With RTT the existing tone scale remains in force. */
    assert(nrf_cs_measurement_confidence(15U, 15U, true, true, 3.0f, 3.0f) == 600U);
    assert(nrf_cs_measurement_confidence(45U, 15U, true, true, 3.0f, 3.0f) == 1000U);
}


static void test_pbr_rtt_disagreement_reduces_confidence(void)
{
    /* Within the RTT error scale: no penalty. */
    assert(nrf_cs_measurement_confidence(45U, 15U, true, true, 3.0f, 5.0f) == 1000U);
    /* 5 m beyond agreement (7.5 m apart): exp(-0.5 * 2^2) = 0.135. */
    const uint16_t disagreeing = nrf_cs_measurement_confidence(45U, 15U, true, true, 3.0f, 10.5f);
    assert(disagreeing >= 130U && disagreeing <= 140U);
    /* A wrapped/multipath phase answer 20 m off is effectively rejected. */
    assert(nrf_cs_measurement_confidence(45U, 15U, true, true, 3.0f, 23.0f) < 5U);
}


static void test_rtt_only_and_nothing(void)
{
    assert(nrf_cs_measurement_confidence(0U, 15U, false, true, NAN, 4.0f) == 400U);
    assert(nrf_cs_measurement_confidence(40U, 15U, false, false, 3.0f, 4.0f) == 0U);
    assert(nrf_cs_measurement_confidence(40U, 15U, true, true, NAN, 4.0f) == 0U);
}


/* A sharp IFFT peak is the evidence of a clean direct path that a missing or wrong coarse RTT cannot give (bench logs 2026-11-03). */
static void test_a_sharp_peak_keeps_a_correct_answer_above_the_production_gate(void)
{
    /* The four attempts that were rejected on the bench: RTT 0 (none), and RTT 6-7 m away from the PBR answer. */
    assert(nrf_cs_measurement_confidence(63U, 15U, true, false, 5.007f, NAN) == 400U);                            /* plain rule */
    assert(nrf_cs_measurement_confidence_with_peak(63U, 15U, true, false, 5.007f, NAN, 28.1f) == 600U);           /* MAIN->G4 */
    assert(nrf_cs_measurement_confidence_with_peak(65U, 15U, true, false, 4.954f, NAN, 33.7f) == 600U);
    assert(nrf_cs_measurement_confidence(69U, 15U, true, true, 4.684f, 10.546f) < 500U);
    assert(nrf_cs_measurement_confidence_with_peak(69U, 15U, true, true, 4.684f, 10.546f, 79.7f) == 600U);       /* RTT 5.9 m away */
    assert(nrf_cs_measurement_confidence_with_peak(47U, 15U, true, true, 4.868f, 11.490f, 26.6f) == 600U);       /* RTT 6.6 m away */

    /* It is a floor, never a cap: a result the plain rule already trusts more keeps its value. */
    assert(nrf_cs_measurement_confidence_with_peak(45U, 15U, true, true, 3.0f, 3.0f, 80.0f) == 1000U);
    assert(nrf_cs_measurement_confidence_with_peak(15U, 15U, true, true, 3.0f, 3.0f, 80.0f) == 600U);

    /* Not sharp (the threshold itself is not sharp), not known, or not enough tones: the plain rule. */
    assert(nrf_cs_measurement_confidence_with_peak(63U, 15U, true, false, 5.0f, NAN, 10.0f) == 400U);
    assert(nrf_cs_measurement_confidence_with_peak(63U, 15U, true, false, 5.0f, NAN, 3.0f) == 400U);
    assert(nrf_cs_measurement_confidence_with_peak(63U, 15U, true, false, 5.0f, NAN, NAN) == 400U);
    assert(nrf_cs_measurement_confidence_with_peak(14U, 15U, true, false, 5.0f, NAN, 80.0f) == 0U);
    assert(nrf_cs_measurement_confidence_with_peak(63U, 0U, true, false, 5.0f, NAN, 80.0f) == 0U);

    /* A phase answer far from a valid RTT stays rejected however sharp the peak is; so does a missing answer. */
    assert(nrf_cs_measurement_confidence_with_peak(45U, 15U, true, true, 3.0f, 23.0f, 80.0f) < 5U);
    assert(nrf_cs_measurement_confidence_with_peak(45U, 15U, true, true, 3.0f, 3.0f + NRF_CS_SHARP_PEAK_RTT_LIMIT_M + 0.1f, 80.0f) < 500U);
    assert(nrf_cs_measurement_confidence_with_peak(45U, 15U, true, true, NAN, 4.0f, 80.0f) == 0U);
    assert(nrf_cs_measurement_confidence_with_peak(45U, 15U, true, true, 3.0f, NAN, 80.0f) == 0U);
    assert(nrf_cs_measurement_confidence_with_peak(40U, 15U, false, true, NAN, 4.0f, 80.0f) == 400U);             /* RTT only: no PBR peak */
    assert(nrf_cs_measurement_confidence_with_peak(40U, 15U, false, false, 3.0f, 4.0f, 80.0f) == 0U);
}


/* The factor that decided the confidence, for the log (why=). Bench 2026-11-03: the same four rejected answers, now named. */
static void test_the_reason_names_what_decided_the_confidence(void)
{
    typedef nrf_cs_confidence_reason_t R;
    assert(nrf_cs_measurement_confidence_reason(63U, 15U, true, false, 5.007f, NAN, 3.0f) == NRF_CS_CONFIDENCE_REASON_RTT_MISSING);
    assert(nrf_cs_measurement_confidence_reason(63U, 15U, true, false, 5.007f, NAN, 28.1f) == NRF_CS_CONFIDENCE_REASON_SHARP_PEAK);
    assert(nrf_cs_measurement_confidence_reason(63U, 15U, true, false, 5.007f, NAN, NAN) == NRF_CS_CONFIDENCE_REASON_RTT_MISSING);
    assert(nrf_cs_measurement_confidence_reason(69U, 15U, true, true, 4.684f, 10.546f, 79.7f) == NRF_CS_CONFIDENCE_REASON_SHARP_PEAK);
    assert(nrf_cs_measurement_confidence_reason(69U, 15U, true, true, 4.684f, 10.546f, 3.0f) == NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES);
    assert(nrf_cs_measurement_confidence_reason(45U, 15U, true, true, 3.0f, 23.0f, 80.0f) == NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES);
    assert(nrf_cs_measurement_confidence_reason(45U, 15U, true, true, 3.0f, 3.5f, 3.0f) == NRF_CS_CONFIDENCE_REASON_OK);
    assert(nrf_cs_measurement_confidence_reason(45U, 15U, true, true, 3.0f, 3.5f, 80.0f) == NRF_CS_CONFIDENCE_REASON_OK);   /* already above the floor */
    assert(nrf_cs_measurement_confidence_reason(14U, 15U, true, false, 5.0f, NAN, 80.0f) == NRF_CS_CONFIDENCE_REASON_TONES_LOW);
    assert(nrf_cs_measurement_confidence_reason(63U, 0U, true, false, 5.0f, NAN, 80.0f) == NRF_CS_CONFIDENCE_REASON_TONES_LOW);
    assert(nrf_cs_measurement_confidence_reason(40U, 15U, false, false, 3.0f, 4.0f, 80.0f) == NRF_CS_CONFIDENCE_REASON_NO_ESTIMATE);
    assert(nrf_cs_measurement_confidence_reason(40U, 15U, false, true, NAN, 4.0f, 80.0f) == NRF_CS_CONFIDENCE_REASON_RTT_ONLY);
    assert(nrf_cs_measurement_confidence_reason(45U, 15U, true, true, NAN, 4.0f, 80.0f) == NRF_CS_CONFIDENCE_REASON_BAD_INPUT);
    assert(nrf_cs_measurement_confidence_reason(45U, 15U, true, true, 3.0f, NAN, 80.0f) == NRF_CS_CONFIDENCE_REASON_BAD_INPUT);
    (void)(R)0;
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_SHARP_PEAK), "sharp_peak") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_RTT_MISSING), "rtt_missing") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_RTT_DISAGREES), "rtt_disagrees") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_TONES_LOW), "tones_low") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_OK), "ok") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name((nrf_cs_confidence_reason_t)99), "unknown") == 0);
}


/* A long run of slots without a tone stays zeros in the IFFT input and can fake a sharp peak: no lift then. */
static void test_a_wide_gap_removes_the_sharp_peak_lift(void)
{
    assert(NRF_CS_IQ_GAP_FILL_LIMIT == 30U);

    /* Below the limit: exactly the function without coverage. */
    assert(nrf_cs_measurement_confidence_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 3U) == 600U);
    assert(nrf_cs_measurement_confidence_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 29U) == 600U);

    /* At the limit and above: the plain rule. */
    assert(nrf_cs_measurement_confidence_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 30U) == 400U);
    assert(nrf_cs_measurement_confidence_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 75U) == 400U);

    /* A disagreeing RTT: lifted with a narrow gap, plain (rejected) with a wide one. */
    assert(nrf_cs_measurement_confidence_with_coverage(69U, 15U, true, true, 4.684f, 10.546f, 79.7f, 3U) == 600U);
    const uint16_t wide = nrf_cs_measurement_confidence_with_coverage(69U, 15U, true, true, 4.684f, 10.546f, 79.7f, 30U);
    assert(wide == nrf_cs_measurement_confidence(69U, 15U, true, true, 4.684f, 10.546f));
    assert(wide < 500U);

    /* A wide gap never lowers a result the plain rule already trusts, and never rescues a rejected one. */
    assert(nrf_cs_measurement_confidence_with_coverage(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 40U) == 1000U);
    assert(nrf_cs_measurement_confidence_with_coverage(14U, 15U, true, false, 5.0f, NAN, 80.0f, 40U) == 0U);

    /* The reason names the gap only where it removed a lift. */
    assert(nrf_cs_measurement_confidence_reason_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 30U) == NRF_CS_CONFIDENCE_REASON_WIDE_GAP);
    assert(nrf_cs_measurement_confidence_reason_with_coverage(63U, 15U, true, false, 5.007f, NAN, 28.1f, 29U) == NRF_CS_CONFIDENCE_REASON_SHARP_PEAK);
    assert(nrf_cs_measurement_confidence_reason_with_coverage(63U, 15U, true, false, 5.0f, NAN, 3.0f, 40U) == NRF_CS_CONFIDENCE_REASON_RTT_MISSING);
    assert(nrf_cs_measurement_confidence_reason_with_coverage(45U, 15U, true, true, 3.0f, 3.5f, 80.0f, 40U) == NRF_CS_CONFIDENCE_REASON_OK);
    assert(nrf_cs_measurement_confidence_reason_with_coverage(14U, 15U, true, false, 5.0f, NAN, 80.0f, 40U) == NRF_CS_CONFIDENCE_REASON_TONES_LOW);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_WIDE_GAP), "wide_gap") == 0);
}


/* Bench logs 2026-11-05 / 2026-11-10: IFFT distances in mm of the stable and the unstable pairs, as the library reported them. */
static void test_ifft_bin_grid_detection(void)
{
    /* On the grid: 5, 15, 16, 17, 18, 19, 24 and 29 bins of 0.29277 m (NFFT 512), all from pairs that jumped between attempts. */
    const float on_grid[] = {1.464f, 4.391f, 4.684f, 4.977f, 5.270f, 5.563f, 7.026f, 8.490f};
    for (size_t i = 0U; i < sizeof(on_grid) / sizeof(on_grid[0]); ++i) {
        assert(nrf_cs_ifft_on_bin_grid(on_grid[i], 512U));
    }
    /* Interpolated values of the stable pairs; 1.457 is the closest, 0.023 bin off the grid. */
    const float off_grid[] = {3.649f, 3.676f, 3.940f, 9.203f, 1.575f, 1.457f, 6.196f, 5.007f};
    for (size_t i = 0U; i < sizeof(off_grid) / sizeof(off_grid[0]); ++i) {
        assert(!nrf_cs_ifft_on_bin_grid(off_grid[i], 512U));
    }
    /* Zero is not a bin, and nothing that is not a distance is on the grid. */
    assert(!nrf_cs_ifft_on_bin_grid(0.0f, 512U));
    assert(!nrf_cs_ifft_on_bin_grid(-1.0f, 512U));
    assert(!nrf_cs_ifft_on_bin_grid(NAN, 512U));
    assert(!nrf_cs_ifft_on_bin_grid(INFINITY, 512U));
    assert(!nrf_cs_ifft_on_bin_grid(1.464f, 0U));
    assert(!nrf_cs_ifft_on_bin_grid(4.684f, 0U));
}


static void test_the_confidence_is_capped_when_the_ifft_answer_is_not_corroborated(void)
{
    typedef nrf_cs_confidence_reason_t R;

    assert(NRF_CS_UNCORROBORATED_PEAK_CONFIDENCE < 500U);

    /* Stable pairs keep their confidence: interpolated IFFT, phase slope within the limit. */
    assert(nrf_cs_measurement_confidence_with_methods(71U, 15U, true, true, 3.676f, 3.389f, 60.0f, 3U, 3.676f, 3.656f, false) == 1000U);
    const uint16_t stable = nrf_cs_measurement_confidence_with_methods(69U, 15U, true, true, 9.187f, 7.027f, 60.0f, 3U, 9.187f, 9.956f, false);
    assert(stable == nrf_cs_measurement_confidence_with_coverage(69U, 15U, true, true, 9.187f, 7.027f, 60.0f, 3U));    /* 0.769 m apart */
    assert(stable >= 500U);
    assert(nrf_cs_measurement_confidence_reason_with_methods(71U, 15U, true, true, 3.676f, 3.389f, 60.0f, 3U, 3.676f, 3.656f, false) == NRF_CS_CONFIDENCE_REASON_OK);

    /* On the grid and 1.8 m from the phase slope: capped, the plain rule alone would have said 1000. */
    assert(nrf_cs_measurement_confidence_with_coverage(72U, 15U, true, true, 1.464f, 1.398f, 54.4f, 3U) == 1000U);
    assert(nrf_cs_measurement_confidence_with_methods(72U, 15U, true, true, 1.464f, 1.398f, 54.4f, 3U, 1.464f, 3.271f, true) == 400U);
    assert(nrf_cs_measurement_confidence_reason_with_methods(72U, 15U, true, true, 1.464f, 1.398f, 54.4f, 3U, 1.464f, 3.271f, true) == NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED);

    /* On the grid alone is enough: the two methods agree. */
    assert(nrf_cs_measurement_confidence_with_methods(72U, 15U, true, true, 5.270f, 5.0f, 110.0f, 3U, 5.270f, 5.911f, true) == 400U);
    assert(nrf_cs_measurement_confidence_reason_with_methods(72U, 15U, true, true, 5.270f, 5.0f, 110.0f, 3U, 5.270f, 5.911f, true) == NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED);

    /* Disagreement alone is enough: not on the grid. */
    assert(nrf_cs_measurement_confidence_with_coverage(72U, 15U, true, true, 1.088f, 0.101f, 31.6f, 3U) == 1000U);
    assert(nrf_cs_measurement_confidence_with_methods(72U, 15U, true, true, 1.088f, 0.101f, 31.6f, 3U, 1.088f, 2.895f, false) == 400U);
    assert(nrf_cs_measurement_confidence_reason_with_methods(72U, 15U, true, true, 1.088f, 0.101f, 31.6f, 3U, 1.088f, 2.895f, false) == NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE);

    /* Exactly at the limit is not a disagreement. */
    assert(nrf_cs_measurement_confidence_with_methods(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 3U, 3.0f, 4.0f, false) == 1000U);
    assert(nrf_cs_measurement_confidence_reason_with_methods(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 3U, 3.0f, 4.0f, false) == NRF_CS_CONFIDENCE_REASON_OK);

    /* An unknown phase slope is not a disagreement. */
    assert(nrf_cs_measurement_confidence_with_methods(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 3U, 3.0f, NAN, false) ==
           nrf_cs_measurement_confidence_with_coverage(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 3U));
    assert(nrf_cs_measurement_confidence_reason_with_methods(45U, 15U, true, true, 3.0f, 3.0f, 80.0f, 3U, 3.0f, NAN, false) == NRF_CS_CONFIDENCE_REASON_OK);

    /* A cap never raises: a result the plain rule already rejects keeps its value and its reason. */
    const uint16_t low = nrf_cs_measurement_confidence_with_coverage(45U, 15U, true, true, 3.0f, 23.0f, 80.0f, 3U);
    assert(low < 400U);
    assert(nrf_cs_measurement_confidence_with_methods(45U, 15U, true, true, 3.0f, 23.0f, 80.0f, 3U, 3.0f, 9.0f, true) == low);
    const R low_reason = nrf_cs_measurement_confidence_reason_with_methods(45U, 15U, true, true, 3.0f, 23.0f, 80.0f, 3U, 3.0f, 9.0f, true);
    assert(low_reason == nrf_cs_measurement_confidence_reason_with_coverage(45U, 15U, true, true, 3.0f, 23.0f, 80.0f, 3U));
    assert(low_reason != NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED && low_reason != NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE);

    /* The cap applies only when best is the IFFT answer. */
    assert(nrf_cs_measurement_confidence_with_methods(45U, 15U, true, true, 2.0f, 2.0f, 80.0f, 3U, 1.464f, 2.0f, true) ==
           nrf_cs_measurement_confidence_with_coverage(45U, 15U, true, true, 2.0f, 2.0f, 80.0f, 3U));
    assert(nrf_cs_measurement_confidence_reason_with_methods(45U, 15U, true, true, 2.0f, 2.0f, 80.0f, 3U, 1.464f, 2.0f, true) == NRF_CS_CONFIDENCE_REASON_OK);

    /* The sharp-peak lift (base 600 here) cannot undo the cap, and the reason names the cap, not the lift. */
    assert(nrf_cs_measurement_confidence_with_coverage(63U, 15U, true, false, 4.684f, NAN, 79.7f, 3U) == 600U);
    assert(nrf_cs_measurement_confidence_with_methods(63U, 15U, true, false, 4.684f, NAN, 79.7f, 3U, 4.684f, 5.916f, true) == 400U);
    const R lifted_reason = nrf_cs_measurement_confidence_reason_with_methods(63U, 15U, true, false, 4.684f, NAN, 79.7f, 3U, 4.684f, 5.916f, true);
    assert(lifted_reason != NRF_CS_CONFIDENCE_REASON_SHARP_PEAK);
    assert(lifted_reason == NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED);

    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_PEAK_COMPENSATED), "peak_compensated") == 0);
    assert(strcmp(nrf_cs_confidence_reason_name(NRF_CS_CONFIDENCE_REASON_METHODS_DISAGREE), "methods_disagree") == 0);
}


int main(void)
{
    test_tone_coverage_scales_confidence();
    test_pbr_rtt_disagreement_reduces_confidence();
    test_rtt_only_and_nothing();
    test_a_sharp_peak_keeps_a_correct_answer_above_the_production_gate();
    test_the_reason_names_what_decided_the_confidence();
    test_a_wide_gap_removes_the_sharp_peak_lift();
    test_ifft_bin_grid_detection();
    test_the_confidence_is_capped_when_the_ifft_answer_is_not_corroborated();
    return 0;
}
