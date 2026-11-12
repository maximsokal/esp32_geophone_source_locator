#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "nrf_cs_distance_method.h"
#include "nrf_geometry_range_offset.h"

static nrf_cs_pair_result_t pair(int32_t distance, int32_t automatic, uint16_t flags, uint16_t method)
{
    nrf_cs_pair_result_t p = {0};
    p.node_a = 0;
    p.node_b = 1;
    p.state = NRF_CS_PAIR_VALID;
    p.distance_mm = distance;
    p.automatic_distance_mm = automatic;
    p.scheduler_quality_flags = flags;
    p.provider_quality_flags = method;
    p.confidence = 731;
    p.attempts = 3;
    p.spread_mm = 47;
    p.diag_distance_mm[0] = 3210;
    return p;
}

static void test_estimate(void)
{
    const uint16_t manual = NRF_CS_QUALITY_MANUAL;
    const uint16_t ifft = NRF_CS_PROVIDER_METHOD_IFFT;
    nrf_cs_pair_result_t p[NRF_CS_MAX_PAIRS] = {pair(2000, 1850, manual, ifft)};
    nrf_geometry_range_offset_t result;
    assert(nrf_geometry_range_offset_estimate(p, 1, &result));
    assert(result.count == 1 && result.offset_mm == -150 && result.span_mm == 0);

    p[0] = pair(2000, 3650, manual, ifft);
    p[1] = pair(2000, 3940, manual, ifft);
    p[2] = pair(2000, 3700, manual, ifft);
    assert(nrf_geometry_range_offset_estimate(p, 3, &result));
    assert(result.count == 3 && result.offset_mm == 1700 && result.span_mm == 290);
    p[3] = pair(2000, 7000, manual, ifft);
    assert(nrf_geometry_range_offset_estimate(p, 4, &result));
    assert(result.count == 4 && result.offset_mm == 1820 && result.span_mm == 3350);

    p[0] = pair(3, 1, manual, ifft);
    p[1] = pair(2, 1, manual, ifft);
    assert(nrf_geometry_range_offset_estimate(p, 2, &result));
    assert(result.count == 2 && result.offset_mm == -1 && result.span_mm == 1);
    p[0] = pair(INT32_MAX, 1, manual, ifft);
    p[1] = pair(1, INT32_MAX, manual, ifft);
    assert(nrf_geometry_range_offset_estimate(p, 2, &result));
    assert(result.count == 2 && result.offset_mm == 0 && result.span_mm == INT64_C(4294967292));

    p[0] = pair(2000, 0, manual, ifft);
    p[1] = pair(2000, 1850, manual, ifft); p[1].state = NRF_CS_PAIR_PENDING;
    p[2] = pair(2000, 1850, 0, ifft);
    p[3] = pair(2000, 1850, manual, NRF_CS_PROVIDER_METHOD_RTT);
    p[4] = pair(2000, 1850, manual, NRF_CS_PROVIDER_METHOD_PHASE);
    p[5] = pair(2000, 1850, manual, ifft | NRF_CS_PROVIDER_METHOD_RTT);
    p[6] = pair(0, 1850, manual, ifft);
    p[7] = pair(-1, 1850, manual, ifft);
    p[8] = pair(2000, -1, manual, ifft);
    p[9] = pair(2000, 1850, manual, 0);
    assert(nrf_geometry_range_offset_estimate(p, 10, &result));
    assert(result.count == 0 && result.offset_mm == 0 && result.span_mm == 0);
    p[0] = pair(2000, 1850, manual | NRF_CS_QUALITY_HAD_RETRY, ifft | 1U);
    assert(nrf_geometry_range_offset_estimate(p, 10, &result));
    assert(result.count == 1 && result.offset_mm == -150);

    p[0] = pair(2000, 1850, manual, ifft);
    p[0].direction_disagreement_mm = NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM + 1;
    assert(nrf_geometry_range_offset_estimate(p, 1, &result));
    assert(result.count == 0 && result.offset_mm == 0 && result.span_mm == 0);
    assert(p[0].distance_mm == 2000 && p[0].automatic_distance_mm == 1850);
    p[0].direction_disagreement_mm = NRF_CS_DIRECTION_DISAGREEMENT_LIMIT_MM;
    assert(nrf_geometry_range_offset_estimate(p, 1, &result));
    assert(result.count == 1 && result.offset_mm == -150);
    p[0].single_direction = true;
    assert(nrf_geometry_range_offset_estimate(p, 1, &result));
    assert(result.count == 0 && p[0].automatic_distance_mm == 1850 && p[0].distance_mm == 2000);
}

static void test_apply(void)
{
    const uint16_t ifft = NRF_CS_PROVIDER_METHOD_IFFT;
    nrf_geometry_node_input_t nodes[] = {{.node_id = 0, .z_mm = 0}, {.node_id = 1, .z_mm = 500}};
    nrf_cs_pair_result_t p[6] = {
        pair(3940, 0, NRF_CS_QUALITY_HAD_RETRY, ifft | 1U),
        pair(2000, 1850, NRF_CS_QUALITY_MANUAL, ifft),
        pair(3940, 0, 0, NRF_CS_PROVIDER_METHOD_PHASE),
        pair(3940, 0, 0, NRF_CS_PROVIDER_METHOD_RTT),
        pair(3940, 0, 0, ifft | NRF_CS_PROVIDER_METHOD_RTT),
        pair(3940, 0, 0, ifft),
    };
    p[5].state = NRF_CS_PAIR_PENDING;
    nrf_cs_pair_result_t original[6], out[6], expected;
    memcpy(original, p, sizeof p);
    assert(nrf_geometry_range_offset_apply(p, 6, nodes, 2, 1650, out));
    memcpy(&expected, &p[0], sizeof expected); expected.distance_mm = 2290;
    assert(memcmp(&out[0], &expected, sizeof expected) == 0);
    for (size_t i = 1; i < 6; ++i) assert(memcmp(&out[i], &p[i], sizeof p[i]) == 0);
    assert(memcmp(original, p, sizeof p) == 0);
    assert(nrf_geometry_range_offset_apply(p, 6, nodes, 2, 0, out));
    assert(memcmp(out, p, sizeof p) == 0);
    p[0].distance_mm = 2140;
    assert(nrf_geometry_range_offset_apply(p, 1, nodes, 2, -150, out));
    assert(out[0].distance_mm == 2290);
    p[0].distance_mm = 2150;
    assert(nrf_geometry_range_offset_apply(p, 1, nodes, 2, 1650, out));
    assert(out[0].state == NRF_CS_PAIR_VALID && out[0].distance_mm == 500);
}

static void test_invalid_corrected_distance(void)
{
    nrf_geometry_node_input_t nodes[] = {{.node_id = 0, .z_mm = INT32_MIN}, {.node_id = 1, .z_mm = INT32_MAX}};
    nrf_cs_pair_result_t p = pair(1000, 0, NRF_CS_QUALITY_HAD_RETRY, NRF_CS_PROVIDER_METHOD_IFFT), out;
    const int32_t distances[] = {1000, 1650, 2149, INT32_MAX, 1000};
    const int32_t offsets[] = {1650, 1650, 1650, -1, INT32_MIN};
    nodes[0].z_mm = 0; nodes[1].z_mm = 500;
    for (size_t i = 0; i < 5; ++i) {
        p.distance_mm = distances[i];
        assert(nrf_geometry_range_offset_apply(&p, 1, nodes, 2, offsets[i], &out));
        assert(out.state == NRF_CS_PAIR_RETRY_EXHAUSTED && out.distance_mm == 0);
        assert((out.scheduler_quality_flags & (NRF_CS_QUALITY_OUTLIER | NRF_CS_QUALITY_HAD_RETRY)) ==
               (NRF_CS_QUALITY_OUTLIER | NRF_CS_QUALITY_HAD_RETRY));
    }
    p.distance_mm = INT32_MAX;
    nodes[0].z_mm = INT32_MIN; nodes[1].z_mm = INT32_MAX;
    assert(nrf_geometry_range_offset_apply(&p, 1, nodes, 2, 0, &out));
    assert(out.state == NRF_CS_PAIR_RETRY_EXHAUSTED && out.distance_mm == 0);
    p.node_b = 4;
    assert(nrf_geometry_range_offset_apply(&p, 1, nodes, 2, 0, &out));
    assert(out.state == NRF_CS_PAIR_RETRY_EXHAUSTED && out.distance_mm == 0);
    assert(out.scheduler_quality_flags & NRF_CS_QUALITY_OUTLIER);
}

static void test_bad_arguments(void)
{
    nrf_cs_pair_result_t p[3] = {pair(2000, 1850, NRF_CS_QUALITY_MANUAL, NRF_CS_PROVIDER_METHOD_IFFT)};
    nrf_cs_pair_result_t before[3], out[3];
    nrf_geometry_node_input_t nodes[] = {{.node_id = 0, .z_mm = 0}, {.node_id = 1, .z_mm = 0}};
    nrf_geometry_range_offset_t estimate = {.offset_mm = 123, .count = 7, .span_mm = 456};
    memcpy(before, p, sizeof p); memcpy(out, p, sizeof p);
    assert(!nrf_geometry_range_offset_estimate(NULL, 1, &estimate));
    assert(!nrf_geometry_range_offset_estimate(p, 0, &estimate));
    assert(!nrf_geometry_range_offset_estimate(p, NRF_CS_MAX_PAIRS + 1, &estimate));
    assert(!nrf_geometry_range_offset_estimate(p, 1, NULL));
    assert(estimate.offset_mm == 123 && estimate.count == 7 && estimate.span_mm == 456);
    assert(!nrf_geometry_range_offset_apply(NULL, 1, nodes, 2, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, 0, nodes, 2, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, NRF_CS_MAX_PAIRS + 1, nodes, 2, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, 1, NULL, 2, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, 1, nodes, 0, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, 1, nodes, NRF_CS_MAX_NODES + 1, 0, out));
    assert(!nrf_geometry_range_offset_apply(p, 1, nodes, 2, 0, NULL));
    assert(!nrf_geometry_range_offset_apply(p, 1, nodes, 2, 0, p));
    assert(!nrf_geometry_range_offset_apply(p, 2, nodes, 2, 0, p + 1));
    assert(memcmp(out, before, sizeof out) == 0 && memcmp(p, before, sizeof p) == 0);
}

int main(void)
{
    test_estimate();
    test_apply();
    test_invalid_corrected_distance();
    test_bad_arguments();
    return 0;
}
