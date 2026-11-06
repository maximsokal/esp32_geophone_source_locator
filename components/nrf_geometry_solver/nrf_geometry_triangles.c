#include "nrf_geometry_triangles.h"

#include <math.h>
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

void nrf_geometry_edge_bounds_cap(nrf_geometry_edge_bounds_t *bounds, int32_t cap_mm)
{
    if (bounds == NULL || cap_mm < 0) return;
    if (bounds->upper_mm > cap_mm) bounds->upper_mm = cap_mm;
    bounds->contradictory = bounds->lower_mm > bounds->upper_mm;
}

#define HULL_MAX_POINTS 16U
#define HULL_MAX_FACES 64U
/* A point nearer than this to a plane (mm) is on it: well below the 1 mm of the positions, far above double rounding. */
#define HULL_ON_PLANE_MM 1e-4

typedef struct {
    double x, y, z;
} vec3_t;

static vec3_t vec_sub(vec3_t a, vec3_t b)
{
    return (vec3_t){a.x - b.x, a.y - b.y, a.z - b.z};
}

static vec3_t vec_cross(vec3_t a, vec3_t b)
{
    return (vec3_t){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static double vec_dot(vec3_t a, vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double plane_cross(const double *u, const double *v, size_t o, size_t a, size_t b)
{
    return (u[a] - u[o]) * (v[b] - v[o]) - (v[a] - v[o]) * (u[b] - u[o]);
}

/* Area of the convex hull of points given in a plane (Andrew's monotone chain). */
static double hull_area_2d(const double *u, const double *v, size_t count)
{
    size_t order[HULL_MAX_POINTS];
    size_t hull[2U * HULL_MAX_POINTS];
    size_t size = 0U;

    for (size_t i = 0U; i < count; ++i) order[i] = i;
    for (size_t i = 1U; i < count; ++i) {            /* insertion sort by u, then v */
        const size_t value = order[i];
        size_t at = i;
        while (at > 0U && (u[order[at - 1U]] > u[value] || (u[order[at - 1U]] == u[value] && v[order[at - 1U]] > v[value]))) {
            order[at] = order[at - 1U];
            --at;
        }
        order[at] = value;
    }
    for (size_t i = 0U; i < count; ++i) {
        while (size >= 2U && plane_cross(u, v, hull[size - 2U], hull[size - 1U], order[i]) <= 0.0) --size;
        hull[size++] = order[i];
    }
    const size_t lower = size + 1U;
    for (size_t i = count - 1U; i > 0U; --i) {
        while (size >= lower && plane_cross(u, v, hull[size - 2U], hull[size - 1U], order[i - 1U]) <= 0.0) --size;
        hull[size++] = order[i - 1U];
    }
    --size;                                           /* the first point is repeated at the end */
    if (size < 3U) return 0.0;
    double twice = 0.0;
    for (size_t i = 0U; i < size; ++i) {
        const size_t a = hull[i], b = hull[(i + 1U) % size];
        twice += u[a] * v[b] - u[b] * v[a];
    }
    return (twice < 0.0 ? -twice : twice) / 2.0;
}

int64_t nrf_geometry_hull_surface_area_mm2(const int32_t *x_mm, const int32_t *y_mm, const int32_t *z_mm, size_t count)
{
    if (x_mm == NULL || y_mm == NULL || z_mm == NULL || count < 3U || count > HULL_MAX_POINTS) return 0;

    vec3_t p[HULL_MAX_POINTS];
    for (size_t i = 0U; i < count; ++i) p[i] = (vec3_t){(double)x_mm[i], (double)y_mm[i], (double)z_mm[i]};

    /* Every face of the hull lies in a plane through three of the points with all the others on one side of it. Each such
     * plane is taken once, however many points it holds, and its polygon is the hull of those points in the plane. */
    vec3_t face_normal[HULL_MAX_FACES];
    double face_offset[HULL_MAX_FACES];
    size_t faces = 0U;
    double total = 0.0;
    for (size_t i = 0U; i + 2U < count; ++i) {
        for (size_t j = i + 1U; j + 1U < count; ++j) {
            for (size_t k = j + 1U; k < count; ++k) {
                vec3_t normal = vec_cross(vec_sub(p[j], p[i]), vec_sub(p[k], p[i]));
                const double length = sqrt(vec_dot(normal, normal));
                if (length < 1.0) continue;            /* collinear: integer positions give 0 or at least 1 mm^2 */
                normal = (vec3_t){normal.x / length, normal.y / length, normal.z / length};
                if (normal.x < 0.0 || (normal.x == 0.0 && (normal.y < 0.0 || (normal.y == 0.0 && normal.z < 0.0)))) {
                    normal = (vec3_t){-normal.x, -normal.y, -normal.z};   /* one orientation per plane */
                }
                const double offset = vec_dot(normal, p[i]);
                bool above = false, below = false;
                size_t on_plane = 0U;
                for (size_t m = 0U; m < count; ++m) {
                    const double side = vec_dot(normal, p[m]) - offset;
                    if (side > HULL_ON_PLANE_MM) above = true;
                    else if (side < -HULL_ON_PLANE_MM) below = true;
                    else ++on_plane;
                }
                if (above && below) continue;          /* the plane cuts the layout: not a face */
                bool known = false;
                for (size_t f = 0U; f < faces && !known; ++f) {
                    known = fabs(vec_dot(face_normal[f], normal) - 1.0) < 1e-9 && fabs(face_offset[f] - offset) < HULL_ON_PLANE_MM;
                }
                if (known || faces >= HULL_MAX_FACES) continue;
                face_normal[faces] = normal;
                face_offset[faces++] = offset;

                /* The polygon of the points in that plane, measured in the coordinate plane that sees it best. */
                double u[HULL_MAX_POINTS], v[HULL_MAX_POINTS];
                size_t held = 0U;
                const double ax = fabs(normal.x), ay = fabs(normal.y), az = fabs(normal.z);
                const double major = ax >= ay && ax >= az ? ax : (ay >= az ? ay : az);
                for (size_t m = 0U; m < count; ++m) {
                    if (fabs(vec_dot(normal, p[m]) - offset) > HULL_ON_PLANE_MM) continue;
                    if (major == ax) { u[held] = p[m].y; v[held] = p[m].z; }
                    else if (major == ay) { u[held] = p[m].x; v[held] = p[m].z; }
                    else { u[held] = p[m].x; v[held] = p[m].y; }
                    ++held;
                }
                const double area = hull_area_2d(u, v, held) / major;
                total += on_plane == count ? 2.0 * area : area;   /* a flat layout shows both faces */
            }
        }
    }
    return total >= 9.2e18 ? INT64_MAX : (int64_t)(total + 0.5);
}

int64_t nrf_geometry_max_hull_surface_area_mm2(size_t vertex_count, int32_t radius_mm)
{
    /* In units of R^2: flat triangle, regular tetrahedron, triangular bipyramid, octahedron; then the whole sphere. */
    static const double inscribed[] = {2.5980762113533160, 4.6188021535170060, 5.8094750193111250, 6.9282032302755090};
    if (vertex_count < 3U || radius_mm <= 0) return 0;
    const double r2 = (double)radius_mm * (double)radius_mm;
    const double factor = vertex_count <= 6U ? inscribed[vertex_count - 3U] : 4.0 * 3.14159265358979323846;
    return (int64_t)(factor * r2 + 0.5);
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
