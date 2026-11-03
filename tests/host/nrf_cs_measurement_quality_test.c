#include <assert.h>
#include <math.h>
#include <string.h>

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


int main(void)
{
    test_tone_coverage_scales_confidence();
    test_pbr_rtt_disagreement_reduces_confidence();
    test_rtt_only_and_nothing();
    test_a_sharp_peak_keeps_a_correct_answer_above_the_production_gate();
    test_the_reason_names_what_decided_the_confidence();
    return 0;
}
