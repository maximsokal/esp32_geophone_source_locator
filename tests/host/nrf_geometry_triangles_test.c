#include <assert.h>
#include <string.h>

#include "nrf_geometry_solver.h"
#include "nrf_geometry_triangles.h"

/* CS-GEO-01: the triangle consistency of the distance matrix, independent of who was MAIN, initiator or reflector. */

static nrf_cs_pair_result_t make_pair(
    nrf_cs_node_id_t a, nrf_cs_node_id_t b, nrf_cs_pair_state_t state, int32_t distance_mm, int32_t spread_mm)
{
    nrf_cs_pair_result_t pair;
    memset(&pair, 0, sizeof(pair));
    pair.node_a = a;
    pair.node_b = b;
    pair.state = state;
    pair.distance_mm = distance_mm;
    pair.spread_mm = spread_mm;
    return pair;
}

#define VALID NRF_CS_PAIR_VALID

/* The six pairs of the ESP32 log 2026-11-02 19:55 (nodes 0, 1, 2, 4): distance and spread as RANGING_PAIR printed them. */
static void bench_pairs(nrf_cs_pair_result_t pairs[6])
{
    pairs[0] = make_pair(0U, 1U, NRF_CS_PAIR_CABLE_EXCEEDED, 5782, 1407);
    pairs[1] = make_pair(0U, 2U, NRF_CS_PAIR_CABLE_EXCEEDED, 5370, 1661);
    pairs[2] = make_pair(0U, 4U, VALID, 2049, 0);
    pairs[3] = make_pair(1U, 2U, VALID, 892, 85);
    pairs[4] = make_pair(1U, 4U, VALID, 4830, 293);
    pairs[5] = make_pair(2U, 4U, VALID, 3343, 62);
}

/* The square 3 m x 4 m: sides 3000 / 4000, diagonals 5000, every spread 40 mm. Pair order: 01 02 03 12 13 23. */
static void square_pairs(nrf_cs_pair_result_t pairs[6])
{
    pairs[0] = make_pair(0U, 1U, VALID, 3000, 40);
    pairs[1] = make_pair(0U, 2U, VALID, 5000, 40);
    pairs[2] = make_pair(0U, 3U, VALID, 4000, 40);
    pairs[3] = make_pair(1U, 2U, VALID, 4000, 40);
    pairs[4] = make_pair(1U, 3U, VALID, 5000, 40);
    pairs[5] = make_pair(2U, 3U, VALID, 3000, 40);
}

static const nrf_geometry_triangle_t *find_triangle(
    const nrf_geometry_triangle_report_t *report, nrf_cs_node_id_t a, nrf_cs_node_id_t b, nrf_cs_node_id_t c)
{
    for (size_t i = 0U; i < report->triangle_count; ++i) {
        const nrf_geometry_triangle_t *t = &report->triangles[i];
        if (t->node_a == a && t->node_b == b && t->node_c == c) return t;
    }
    return NULL;
}

/* The distances the Bluetooth cycle reported: 1-4 is 4830 mm, but 1-2 and 2-4 add up to 4235 mm. The audit called this
 * "impossible"; with the spreads of the three sides the excess of 595 mm is inside the tolerance, so it is reported as
 * marginal and not as a failure. It is still visible, with the number. */
static void test_the_bench_triple_is_measured_not_dismissed(void)
{
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_triangle_report_t report;

    bench_pairs(pairs);
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.triangle_count == 4U);                         /* C(4,3): every triple of the four nodes is complete */
    const nrf_geometry_triangle_t *t = find_triangle(&report, 1U, 2U, 4U);
    assert(t != NULL);
    assert(t->distance_ab_mm == 892 && t->distance_ac_mm == 4830 && t->distance_bc_mm == 3343);
    assert(t->excess_mm == 595);                                 /* 4830 - (892 + 3343) */
    assert(t->tolerance_mm == NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM);   /* 2 * sqrt(85^2 + 293^2 + 62^2) = 622 < 750 */
    assert(t->status == NRF_GEOMETRY_TRIANGLE_MARGINAL);
    assert(report.marginal_count == 1U && report.inconsistent_count == 0U);
    assert(report.worst_excess_mm == 595);
    assert(report.suspect_pair == -1);

    /* The other three close, with room: -480, -1097 and -22 mm. */
    assert(find_triangle(&report, 0U, 1U, 2U)->excess_mm == -480);
    assert(find_triangle(&report, 0U, 1U, 4U)->excess_mm == -1097);
    assert(find_triangle(&report, 0U, 2U, 4U)->excess_mm == -22);
    assert(find_triangle(&report, 0U, 2U, 4U)->status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);

    /* What the two good sides allow for the third: 1-4 lies between 3733 mm (via MAIN) and 4235 mm (via node 2). */
    nrf_geometry_edge_bounds_t bounds;
    assert(nrf_geometry_triangle_bounds(pairs, 6U, &report, 1U, 4U, &bounds));
    assert(bounds.witnesses == 2U);
    assert(bounds.lower_mm == 3733 && bounds.upper_mm == 4235 && !bounds.contradictory);
    assert(pairs[4].distance_mm > bounds.upper_mm);              /* the measured 4830 mm is 595 mm outside it */
}

/* The check looks at three nodes, not at who measured whom: reversing the node order of every pair and the order of the
 * pairs gives the same triangles. */
static void test_the_result_does_not_depend_on_roles_or_order(void)
{
    nrf_cs_pair_result_t pairs[6];
    nrf_cs_pair_result_t swapped[6];
    nrf_geometry_triangle_report_t a;
    nrf_geometry_triangle_report_t b;

    bench_pairs(pairs);
    for (size_t i = 0U; i < 6U; ++i) {
        swapped[5U - i] = pairs[i];
        swapped[5U - i].node_a = pairs[i].node_b;
        swapped[5U - i].node_b = pairs[i].node_a;
    }
    assert(nrf_geometry_triangles_check(pairs, 6U, &a));
    assert(nrf_geometry_triangles_check(swapped, 6U, &b));
    assert(a.triangle_count == b.triangle_count && a.marginal_count == b.marginal_count);
    assert(a.inconsistent_count == b.inconsistent_count && a.worst_excess_mm == b.worst_excess_mm);
    for (size_t i = 0U; i < a.triangle_count; ++i) {
        const nrf_geometry_triangle_t *x = &a.triangles[i];
        const nrf_geometry_triangle_t *y = find_triangle(&b, x->node_a, x->node_b, x->node_c);
        assert(y != NULL && x->excess_mm == y->excess_mm && x->status == y->status && x->tolerance_mm == y->tolerance_mm);
        assert(x->distance_ab_mm == y->distance_ab_mm && x->distance_ac_mm == y->distance_ac_mm &&
               x->distance_bc_mm == y->distance_bc_mm);
    }
    /* Node ids need not start at 0 or be consecutive. */
    nrf_cs_pair_result_t odd[3] = {
        make_pair(40U, 7U, VALID, 3000, 40), make_pair(7U, 19U, VALID, 4000, 40), make_pair(19U, 40U, VALID, 5000, 40),
    };
    assert(nrf_geometry_triangles_check(odd, 3U, &a) && a.triangle_count == 1U);
    assert(a.triangles[0].node_a == 7U && a.triangles[0].node_b == 19U && a.triangles[0].node_c == 40U);
    assert(a.triangles[0].excess_mm == -2000 && a.triangles[0].status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
}

static void test_a_gross_contradiction_is_inconsistent_and_the_shared_side_is_named(void)
{
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_triangle_report_t report;

    square_pairs(pairs);
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.triangle_count == 4U && report.inconsistent_count == 0U && report.marginal_count == 0U);
    assert(report.worst_excess_mm <= 0 && report.suspect_pair == -1);

    /* One diagonal 4.5 m too long: it belongs to two triangles, both break; no other side belongs to both. */
    pairs[1].distance_mm = 9500;
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.inconsistent_count == 2U);
    assert(find_triangle(&report, 0U, 1U, 2U)->status == NRF_GEOMETRY_TRIANGLE_INCONSISTENT);
    assert(find_triangle(&report, 0U, 2U, 3U)->status == NRF_GEOMETRY_TRIANGLE_INCONSISTENT);
    assert(find_triangle(&report, 0U, 1U, 2U)->excess_mm == 2500);        /* 9500 - (3000 + 4000) */
    assert(find_triangle(&report, 0U, 1U, 3U)->status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
    assert(find_triangle(&report, 1U, 2U, 3U)->status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
    assert(report.pair_violations[1] == 2U);
    assert(report.pair_violations[0] == 1U && report.pair_violations[3] == 1U);
    assert(report.pair_violations[2] == 1U && report.pair_violations[5] == 1U && report.pair_violations[4] == 0U);
    assert(report.suspect_pair == 1);                            /* pair 0-2 */
    assert(report.worst_excess_mm == 2500);

    /* A single violated triangle blames no side: all three stay suspects together. */
    square_pairs(pairs);
    pairs[3].distance_mm = 9000;                                 /* 1-2: triangles 0-1-2 and 1-2-3 both break */
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.inconsistent_count == 2U && report.suspect_pair == 3);

    nrf_cs_pair_result_t three[3] = {
        make_pair(0U, 1U, VALID, 1000, 0), make_pair(0U, 2U, VALID, 1000, 0), make_pair(1U, 2U, VALID, 6000, 0),
    };
    assert(nrf_geometry_triangles_check(three, 3U, &report));
    assert(report.triangle_count == 1U && report.inconsistent_count == 1U);
    assert(report.pair_violations[0] == 1U && report.pair_violations[1] == 1U && report.pair_violations[2] == 1U);
    assert(report.suspect_pair == -1);                           /* it cannot be told which of the three is wrong */

    /* Two opposite sides far too long break every triangle once each side is in two of them: a tie, nobody is named. */
    square_pairs(pairs);
    pairs[0].distance_mm = 12000;                                /* 0-1 */
    pairs[5].distance_mm = 12000;                                /* 2-3 */
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.inconsistent_count == 4U);
    for (size_t p = 0U; p < 6U; ++p) assert(report.pair_violations[p] == 2U);
    assert(report.suspect_pair == -1);
}

static void test_the_tolerance_edge_is_exact_and_follows_the_spread(void)
{
    nrf_cs_pair_result_t pairs[3];
    nrf_geometry_triangle_report_t report;

    /* Spreads 0: the tolerance is the solver's 750 mm. A side of 2000 closes exactly; each millimetre more is excess. */
    pairs[0] = make_pair(0U, 1U, VALID, 1000, 0);
    pairs[1] = make_pair(0U, 2U, VALID, 1000, 0);
    pairs[2] = make_pair(1U, 2U, VALID, 2000, 0);
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].excess_mm == 0 && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
    pairs[2].distance_mm = 2001;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].excess_mm == 1 && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_MARGINAL);
    pairs[2].distance_mm = 2000 + NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_MARGINAL && report.inconsistent_count == 0U);
    pairs[2].distance_mm = 2000 + NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM + 1;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_INCONSISTENT && report.inconsistent_count == 1U);
    assert(report.worst_excess_mm == NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM + 1);

    /* Noisy sides widen the tolerance: spread 1000 on one side gives 2 * 1000 = 2000 mm. */
    pairs[0].spread_mm = 1000;
    pairs[2].distance_mm = 2000 + 2000;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].tolerance_mm == 2000 && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_MARGINAL);
    pairs[2].distance_mm = 2000 + 2001;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_INCONSISTENT);

    /* A quiet triangle never gets less than the solver's tolerance. */
    pairs[0].spread_mm = 5;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].tolerance_mm == NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM);
}

/* Three sides that are all far too long by the same factor still close: the check cannot see a common bias and must not
 * be read as a proof of accuracy. Nothing in the report says "accurate". */
static void test_a_common_bias_passes_the_triangles(void)
{
    nrf_cs_pair_result_t truth[3] = {
        make_pair(0U, 1U, VALID, 1000, 30), make_pair(0U, 2U, VALID, 1000, 30), make_pair(1U, 2U, VALID, 1000, 30),
    };
    nrf_cs_pair_result_t biased[3];
    nrf_geometry_triangle_report_t report;

    memcpy(biased, truth, sizeof(biased));
    for (size_t i = 0U; i < 3U; ++i) biased[i].distance_mm = 3000;      /* the 3.7 m-for-2 m errors of the bench */
    assert(nrf_geometry_triangles_check(truth, 3U, &report) && report.inconsistent_count == 0U);
    assert(nrf_geometry_triangles_check(biased, 3U, &report));
    assert(report.triangle_count == 1U && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
    assert(report.triangles[0].excess_mm == -3000 && report.inconsistent_count == 0U && report.marginal_count == 0U);
    /* A constant offset on every side of a 3-4-5 triangle also closes. */
    nrf_cs_pair_result_t offset[3] = {
        make_pair(0U, 1U, VALID, 3000 + 1500, 30), make_pair(0U, 2U, VALID, 4000 + 1500, 30),
        make_pair(1U, 2U, VALID, 5000 + 1500, 30),
    };
    assert(nrf_geometry_triangles_check(offset, 3U, &report) && report.inconsistent_count == 0U);
}

static void test_a_partial_graph_gives_an_interval_not_a_distance(void)
{
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_triangle_report_t report;
    nrf_geometry_edge_bounds_t bounds;

    /* 1-4 never measured: the sides to nodes 0 and 2 bound it, the failed pair's own 0 mm is not used. */
    bench_pairs(pairs);
    pairs[4] = make_pair(1U, 4U, NRF_CS_PAIR_RETRY_EXHAUSTED, 0, 0);
    assert(nrf_geometry_triangles_check(pairs, 6U, &report));
    assert(report.triangle_count == 2U);                         /* only 0-1-2 and 0-2-4 are complete without 1-4 */
    assert(find_triangle(&report, 1U, 2U, 4U) == NULL && find_triangle(&report, 0U, 1U, 4U) == NULL);
    assert(nrf_geometry_triangle_bounds(pairs, 6U, &report, 1U, 4U, &bounds));
    assert(bounds.witnesses == 2U && bounds.lower_mm == 3733 && bounds.upper_mm == 4235 && !bounds.contradictory);
    assert(nrf_geometry_triangle_bounds(pairs, 6U, &report, 4U, 1U, &bounds));      /* the order of the ends is free */
    assert(bounds.lower_mm == 3733 && bounds.upper_mm == 4235);

    /* One witness: the plain triangle interval |a - b| .. a + b. */
    nrf_cs_pair_result_t one[2] = { make_pair(0U, 1U, VALID, 3000, 0), make_pair(0U, 2U, VALID, 4500, 0) };
    assert(nrf_geometry_triangle_bounds(one, 2U, NULL, 1U, 2U, &bounds));
    assert(bounds.witnesses == 1U && bounds.lower_mm == 1500 && bounds.upper_mm == 7500);

    /* No witness: nothing is known, and the answer says so instead of inventing a length. */
    assert(nrf_geometry_triangle_bounds(one, 2U, NULL, 1U, 7U, &bounds));
    assert(bounds.witnesses == 0U && bounds.lower_mm == 0 && bounds.upper_mm == INT32_MAX && !bounds.contradictory);
    assert(nrf_geometry_triangle_bounds(NULL, 0U, NULL, 1U, 2U, &bounds) && bounds.witnesses == 0U);

    /* Witnesses that contradict each other. */
    nrf_cs_pair_result_t clash[4] = {
        make_pair(0U, 1U, VALID, 1000, 0), make_pair(0U, 2U, VALID, 1100, 0),      /* 1-2 in 100 .. 2100 via node 0 */
        make_pair(3U, 1U, VALID, 5000, 0), make_pair(3U, 2U, VALID, 1000, 0),      /* 1-2 in 4000 .. 6000 via node 3 */
    };
    assert(nrf_geometry_triangle_bounds(clash, 4U, NULL, 1U, 2U, &bounds));
    assert(bounds.witnesses == 2U && bounds.lower_mm == 4000 && bounds.upper_mm == 2100 && bounds.contradictory);
}

/* The sides of a violated triangle are suspects, not witnesses: they do not bound anything else. */
static void test_suspect_sides_do_not_bound_other_sides(void)
{
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_triangle_report_t report;
    nrf_geometry_edge_bounds_t bounds;

    square_pairs(pairs);
    pairs[1].distance_mm = 9500;                                 /* 0-2 breaks triangles 0-1-2 and 0-2-3 */
    assert(nrf_geometry_triangles_check(pairs, 6U, &report) && report.inconsistent_count == 2U);

    /* 1-3 through node 0 needs 0-1 and 0-3 (both in a violated triangle) and through node 2 needs 1-2 and 2-3 (same). */
    assert(nrf_geometry_triangle_bounds(pairs, 6U, &report, 1U, 3U, &bounds));
    assert(bounds.witnesses == 0U && bounds.upper_mm == INT32_MAX);
    /* Trusting everything would let the broken diagonal in: the bound is then wrong. */
    assert(nrf_geometry_triangle_bounds(pairs, 6U, NULL, 1U, 3U, &bounds));
    assert(bounds.witnesses == 2U);
}

static void test_only_pairs_with_a_measured_distance_take_part(void)
{
    static const nrf_cs_pair_state_t unusable[] = {
        NRF_CS_PAIR_UNKNOWN, NRF_CS_PAIR_PENDING, NRF_CS_PAIR_MISSING, NRF_CS_PAIR_RETRY_EXHAUSTED,
        NRF_CS_PAIR_DIRECTIONS_DISAGREE,
    };
    nrf_cs_pair_result_t pair = make_pair(0U, 1U, VALID, 1000, 0);
    nrf_cs_pair_result_t pairs[3];
    nrf_geometry_triangle_report_t report;

    assert(nrf_geometry_triangle_pair_usable(&pair));
    pair.state = NRF_CS_PAIR_CABLE_EXCEEDED;                     /* measured, only longer than the cable */
    assert(nrf_geometry_triangle_pair_usable(&pair));
    pair.state = VALID;
    pair.distance_mm = -1;
    assert(!nrf_geometry_triangle_pair_usable(&pair));
    assert(!nrf_geometry_triangle_pair_usable(NULL));
    for (size_t i = 0U; i < sizeof(unusable) / sizeof(unusable[0]); ++i) {
        pair = make_pair(0U, 1U, unusable[i], 1000, 0);
        assert(!nrf_geometry_triangle_pair_usable(&pair));
        pairs[0] = pair;
        pairs[1] = make_pair(0U, 2U, VALID, 1000, 0);
        pairs[2] = make_pair(1U, 2U, VALID, 9000, 0);
        assert(nrf_geometry_triangles_check(pairs, 3U, &report));
        assert(report.triangle_count == 0U && report.inconsistent_count == 0U);   /* the third side cannot be judged */
    }
}

static void test_bad_arguments_and_extreme_values_are_safe(void)
{
    nrf_cs_pair_result_t pairs[3];
    nrf_cs_pair_result_t many[NRF_CS_MAX_PAIRS + 1U];
    nrf_geometry_triangle_report_t report;
    nrf_geometry_edge_bounds_t bounds;

    assert(!nrf_geometry_triangles_check(NULL, 0U, NULL));
    assert(!nrf_geometry_triangles_check(NULL, 3U, &report) && report.triangle_count == 0U && report.suspect_pair == -1);
    memset(many, 0, sizeof(many));
    assert(!nrf_geometry_triangles_check(many, NRF_CS_MAX_PAIRS + 1U, &report));
    assert(nrf_geometry_triangles_check(NULL, 0U, &report) && report.triangle_count == 0U && report.suspect_pair == -1);
    assert(!nrf_geometry_triangle_bounds(pairs, 0U, NULL, 1U, 1U, &bounds));        /* a side needs two different ends */
    assert(!nrf_geometry_triangle_bounds(pairs, 0U, NULL, 1U, 2U, NULL));
    assert(strcmp(nrf_geometry_triangle_status_name(NRF_GEOMETRY_TRIANGLE_CONSISTENT), "consistent") == 0);
    assert(strcmp(nrf_geometry_triangle_status_name(NRF_GEOMETRY_TRIANGLE_MARGINAL), "marginal") == 0);
    assert(strcmp(nrf_geometry_triangle_status_name(NRF_GEOMETRY_TRIANGLE_INCONSISTENT), "inconsistent") == 0);
    assert(strcmp(nrf_geometry_triangle_status_name((nrf_geometry_triangle_status_t)9), "?") == 0);

    /* The largest legal distances and spreads: no overflow, no false alarm for sides that close. */
    pairs[0] = make_pair(0U, 1U, VALID, INT32_MAX, INT32_MAX);
    pairs[1] = make_pair(0U, 2U, VALID, INT32_MAX, INT32_MAX);
    pairs[2] = make_pair(1U, 2U, VALID, INT32_MAX, INT32_MAX);
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].excess_mm < 0 && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_CONSISTENT);
    assert(report.triangles[0].tolerance_mm > 0);
    /* One huge side against two tiny ones: a clear violation, the excess clamps instead of wrapping. */
    pairs[0].distance_mm = 1;
    pairs[1].distance_mm = 1;
    pairs[0].spread_mm = 0;
    pairs[1].spread_mm = 0;
    pairs[2].spread_mm = 0;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report));
    assert(report.triangles[0].excess_mm == INT32_MAX - 2 && report.triangles[0].status == NRF_GEOMETRY_TRIANGLE_INCONSISTENT);
    /* A negative spread (never produced) counts as 0. */
    pairs[2].spread_mm = -5;
    pairs[2].distance_mm = 2;
    assert(nrf_geometry_triangles_check(pairs, 3U, &report) && report.triangles[0].excess_mm == 0);
    assert(report.triangles[0].tolerance_mm == NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM);
}

/* Five nodes, ten pairs, ten triangles: the largest network the firmware supports fits the report. */
static void test_the_largest_network_fits_the_report(void)
{
    nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
    nrf_geometry_triangle_report_t report;
    size_t n = 0U;

    for (nrf_cs_node_id_t a = 0U; a < NRF_CS_MAX_NODES; ++a) {
        for (nrf_cs_node_id_t b = (nrf_cs_node_id_t)(a + 1U); b < NRF_CS_MAX_NODES; ++b) {
            pairs[n++] = make_pair(a, b, VALID, 1000 + 10 * (a + b), 20);
        }
    }
    assert(n == NRF_CS_MAX_PAIRS);
    assert(nrf_geometry_triangles_check(pairs, n, &report));
    assert(report.triangle_count == NRF_GEOMETRY_MAX_TRIANGLES && report.inconsistent_count == 0U);
}

int main(void)
{
    test_the_bench_triple_is_measured_not_dismissed();
    test_the_result_does_not_depend_on_roles_or_order();
    test_a_gross_contradiction_is_inconsistent_and_the_shared_side_is_named();
    test_the_tolerance_edge_is_exact_and_follows_the_spread();
    test_a_common_bias_passes_the_triangles();
    test_a_partial_graph_gives_an_interval_not_a_distance();
    test_suspect_sides_do_not_bound_other_sides();
    test_only_pairs_with_a_measured_distance_take_part();
    test_bad_arguments_and_extreme_values_are_safe();
    test_the_largest_network_fits_the_report();
    return 0;
}
