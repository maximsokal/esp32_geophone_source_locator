#include <assert.h>
#include <math.h>

#include "nrf_cs_measurement_quality.h"
#include "nrf_cs_scheduler.h"


static void test_tone_coverage_scales_confidence(void)
{
    /* Below threshold: the phase estimate is not trusted. */
    assert(nrf_cs_measurement_confidence(14U, 15U, true, false, 3.0f, NAN) == 0U);
    /* At threshold, no RTT cross-check: 0.6. */
    assert(nrf_cs_measurement_confidence(15U, 15U, true, false, 3.0f, NAN) == 600U);
    /* Three times the threshold or more: full tone factor. */
    assert(nrf_cs_measurement_confidence(45U, 15U, true, false, 3.0f, NAN) == 1000U);
    assert(nrf_cs_measurement_confidence(70U, 15U, true, false, 3.0f, NAN) == 1000U);
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


int main(void)
{
    test_tone_coverage_scales_confidence();
    test_pbr_rtt_disagreement_reduces_confidence();
    test_rtt_only_and_nothing();
    return 0;
}
