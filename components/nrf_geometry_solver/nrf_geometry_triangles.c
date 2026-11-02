#include "nrf_geometry_triangles.h"

#include <string.h>

#include "nrf_geometry_solver.h"

/* A spread above this is capped before it is squared, so three of them cannot overflow 64 bits. */
#define SPREAD_CAP_MM 1000000

static int64_t isqrt64(int64_t value)
{
    int64_t low = 0;
    int64_t high = 3037000499LL;    /* floor(sqrt(2^63 - 1)) */

    if (value <= 0) return 0;
    while (low < high) {
        const int64_t mid = low + (high - low + 1) / 2;
        if (mid <= value / mid) low = mid; else high = mid - 1;
    }
    return low;
}

static int32_t clamp_i32(int64_t value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

bool nrf_geometry_triangle_pair_usable(const nrf_cs_pair_result_t *pair)
{
    return pair != NULL && pair->distance_mm >= 0 &&
        (pair->state == NRF_CS_PAIR_VALID || pair->state == NRF_CS_PAIR_CABLE_EXCEEDED);
}

const char *nrf_geometry_triangle_status_name(nrf_geometry_triangle_status_t status)
{
    switch (status) {
        case NRF_GEOMETRY_TRIANGLE_CONSISTENT: return "consistent";
        case NRF_GEOMETRY_TRIANGLE_MARGINAL: return "marginal";
        case NRF_GEOMETRY_TRIANGLE_INCONSISTENT: return "inconsistent";
        default: return "?";
    }
}

/* Index of the pair between two nodes, in either order; -1 when there is none. */
static int find_pair(const nrf_cs_pair_result_t *pairs, size_t pair_count, nrf_cs_node_id_t a, nrf_cs_node_id_t b)
{
    for (size_t i = 0U; i < pair_count; ++i) {
        if ((pairs[i].node_a == a && pairs[i].node_b == b) || (pairs[i].node_a == b && pairs[i].node_b == a)) {
            return (int)i;
        }
    }
    return -1;
}

/* The distinct nodes that appear in the pairs, ascending. */
static size_t collect_nodes(const nrf_cs_pair_result_t *pairs, size_t pair_count, nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES])
{
    size_t count = 0U;

    for (size_t i = 0U; i < pair_count; ++i) {
        const nrf_cs_node_id_t ends[2] = { pairs[i].node_a, pairs[i].node_b };
        for (size_t e = 0U; e < 2U; ++e) {
            size_t at = 0U;
            while (at < count && nodes[at] < ends[e]) ++at;
            if (at < count && nodes[at] == ends[e]) continue;
            if (count >= NRF_CS_MAX_NODES) return count;
            memmove(&nodes[at + 1U], &nodes[at], (count - at) * sizeof(nodes[0]));
            nodes[at] = ends[e];
            ++count;
        }
    }
    return count;
}

static int32_t tolerance_for(const nrf_cs_pair_result_t *ab, const nrf_cs_pair_result_t *ac, const nrf_cs_pair_result_t *bc)
{
    int64_t sum_squares = 0;
    const nrf_cs_pair_result_t *sides[3] = { ab, ac, bc };

    for (size_t i = 0U; i < 3U; ++i) {
        int64_t spread = sides[i]->spread_mm < 0 ? 0 : sides[i]->spread_mm;
        if (spread > SPREAD_CAP_MM) spread = SPREAD_CAP_MM;
        sum_squares += spread * spread;
    }
    const int64_t statistical = 2 * isqrt64(sum_squares);
    return clamp_i32(statistical > NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM ? statistical : NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM);
}

bool nrf_geometry_triangles_check(
    const nrf_cs_pair_result_t *pairs, size_t pair_count, nrf_geometry_triangle_report_t *report)
{
    nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];

    if (report == NULL) return false;
    memset(report, 0, sizeof(*report));
    report->suspect_pair = -1;
    if ((pair_count > 0U && pairs == NULL) || pair_count > NRF_CS_MAX_PAIRS) return false;

    const size_t node_count = collect_nodes(pairs, pair_count, nodes);
    for (size_t i = 0U; i + 2U < node_count; ++i) {
        for (size_t j = i + 1U; j + 1U < node_count; ++j) {
            for (size_t k = j + 1U; k < node_count; ++k) {
                const int ab = find_pair(pairs, pair_count, nodes[i], nodes[j]);
                const int ac = find_pair(pairs, pair_count, nodes[i], nodes[k]);
                const int bc = find_pair(pairs, pair_count, nodes[j], nodes[k]);
                if (ab < 0 || ac < 0 || bc < 0 ||
                    !nrf_geometry_triangle_pair_usable(&pairs[ab]) || !nrf_geometry_triangle_pair_usable(&pairs[ac]) ||
                    !nrf_geometry_triangle_pair_usable(&pairs[bc]) || report->triangle_count >= NRF_GEOMETRY_MAX_TRIANGLES) {
                    continue;
                }
                nrf_geometry_triangle_t *t = &report->triangles[report->triangle_count++];
                t->node_a = nodes[i];
                t->node_b = nodes[j];
                t->node_c = nodes[k];
                t->distance_ab_mm = pairs[ab].distance_mm;
                t->distance_ac_mm = pairs[ac].distance_mm;
                t->distance_bc_mm = pairs[bc].distance_mm;
                t->pair_index[0] = (uint8_t)ab;
                t->pair_index[1] = (uint8_t)ac;
                t->pair_index[2] = (uint8_t)bc;
                t->tolerance_mm = tolerance_for(&pairs[ab], &pairs[ac], &pairs[bc]);

                const int64_t sides[3] = { t->distance_ab_mm, t->distance_ac_mm, t->distance_bc_mm };
                int64_t longest = sides[0];
                for (size_t s = 1U; s < 3U; ++s) if (sides[s] > longest) longest = sides[s];
                t->excess_mm = clamp_i32(2 * longest - (sides[0] + sides[1] + sides[2]));
                if (t->excess_mm > report->worst_excess_mm) report->worst_excess_mm = t->excess_mm;
                if (t->excess_mm <= 0) {
                    t->status = NRF_GEOMETRY_TRIANGLE_CONSISTENT;
                } else if (t->excess_mm <= t->tolerance_mm) {
                    t->status = NRF_GEOMETRY_TRIANGLE_MARGINAL;
                    ++report->marginal_count;
                } else {
                    t->status = NRF_GEOMETRY_TRIANGLE_INCONSISTENT;
                    ++report->inconsistent_count;
                    for (size_t s = 0U; s < 3U; ++s) ++report->pair_violations[t->pair_index[s]];
                }
            }
        }
    }

    /* A pair is named only when it is in at least two violated triangles and in more of them than any other pair. */
    unsigned best = 0U;
    bool tie = false;
    for (size_t p = 0U; p < pair_count; ++p) {
        if (report->pair_violations[p] > best) {
            best = report->pair_violations[p];
            report->suspect_pair = (int)p;
            tie = false;
        } else if (report->pair_violations[p] == best && best > 0U) {
            tie = true;
        }
    }
    if (best < 2U || tie) report->suspect_pair = -1;
    return true;
}

bool nrf_geometry_triangle_bounds(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const nrf_geometry_triangle_report_t *report,
    nrf_cs_node_id_t node_a,
    nrf_cs_node_id_t node_b,
    nrf_geometry_edge_bounds_t *bounds)
{
    nrf_cs_node_id_t nodes[NRF_CS_MAX_NODES];

    if (bounds == NULL) return false;
    memset(bounds, 0, sizeof(*bounds));
    bounds->upper_mm = INT32_MAX;
    if ((pair_count > 0U && pairs == NULL) || pair_count > NRF_CS_MAX_PAIRS || node_a == node_b) return false;

    int64_t lower = 0;
    int64_t upper = INT32_MAX;
    const size_t node_count = collect_nodes(pairs, pair_count, nodes);
    for (size_t n = 0U; n < node_count; ++n) {
        if (nodes[n] == node_a || nodes[n] == node_b) continue;
        const int first = find_pair(pairs, pair_count, node_a, nodes[n]);
        const int second = find_pair(pairs, pair_count, node_b, nodes[n]);
        if (first < 0 || second < 0 || !nrf_geometry_triangle_pair_usable(&pairs[first]) ||
            !nrf_geometry_triangle_pair_usable(&pairs[second])) {
            continue;
        }
        if (report != NULL && (report->pair_violations[first] > 0U || report->pair_violations[second] > 0U)) {
            continue;       /* a side of a violated triangle is a suspect, not a witness */
        }
        const int64_t d1 = pairs[first].distance_mm;
        const int64_t d2 = pairs[second].distance_mm;
        const int64_t low = d1 > d2 ? d1 - d2 : d2 - d1;
        if (low > lower) lower = low;
        if (d1 + d2 < upper) upper = d1 + d2;
        ++bounds->witnesses;
    }
    bounds->lower_mm = clamp_i32(lower);
    bounds->upper_mm = clamp_i32(upper);
    bounds->contradictory = bounds->witnesses > 0U && lower > upper;
    return true;
}
