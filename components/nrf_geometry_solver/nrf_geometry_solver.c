#include "nrf_geometry_solver.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>


static bool rounded_position_fits_i32(double value)
{
    return
        isfinite(value) &&
        value >= (double)INT32_MIN - 0.5 &&
        value <= (double)INT32_MAX + 0.5;
}

/* Millimeter-scale distances as double; ESP32-S3 has a hardware FPU and
 * this is a low-rate, small-N (<=5 nodes) computation, so there is no
 * fixed-point-only constraint here the way there is on the nRF side. */

static bool find_pair_distance(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_cs_node_id_t a,
    nrf_cs_node_id_t b,
    double *out_distance_mm)
{
    for (size_t i = 0; i < pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = &pairs[i];
        if (pair->state != NRF_CS_PAIR_VALID) {
            continue;
        }
        const bool forward = pair->node_a == a && pair->node_b == b;
        const bool reverse = pair->node_a == b && pair->node_b == a;
        if (forward || reverse) {
            *out_distance_mm = (double)pair->distance_mm;
            return true;
        }
    }
    return false;
}

#define TOLERANCE_MM ((double)NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM)

/*
 * Horizontal (X/Y-plane) distance from a known slant distance and the
 * already-known vertical separation. Returns false when the pair is
 * physically impossible (slant shorter than |dz| by more than the
 * tolerance) -- such a pair must not be used as a reference at all, rather
 * than silently collapsing to a zero horizontal distance (MAX-205). A
 * slightly negative radicand within the tolerance is noise and clamps to 0.
 */
static bool horizontal_distance_mm(double slant_mm, double z_diff_mm, double *out_mm)
{
    if (slant_mm + TOLERANCE_MM < fabs(z_diff_mm)) {
        return false;
    }
    const double radicand = slant_mm * slant_mm - z_diff_mm * z_diff_mm;
    *out_mm = radicand > 0.0 ? sqrt(radicand) : 0.0;
    return true;
}

/*
 * Two circles of radius ra/rb centered at (ax,ay)/(bx,by). Writes the two
 * intersection points (identical when tangent). Circles that miss each
 * other -- externally or one inside the other -- by at most the tolerance
 * are treated as tangent at the midpoint of their closest approach (range
 * noise); a larger miss, or coincident centers, returns false.
 */
static bool circle_intersect(
    double ax, double ay, double ra,
    double bx, double by, double rb,
    double *p1x, double *p1y,
    double *p2x, double *p2y)
{
    const double dx = bx - ax;
    const double dy = by - ay;
    const double d = sqrt(dx * dx + dy * dy);

    if (d < 1.0) {
        return false;
    }

    const double ux = dx / d;
    const double uy = dy / d;
    double along = 0.0;

    if (d > ra + rb) {
        const double gap = d - (ra + rb);
        if (gap > TOLERANCE_MM) {
            return false;
        }
        along = ra + gap / 2.0;
    } else if (d < fabs(ra - rb)) {
        if (ra > rb) {
            const double gap = ra - (d + rb);
            if (gap > TOLERANCE_MM) {
                return false;
            }
            along = ra - gap / 2.0;
        } else {
            const double gap = rb - (d + ra);
            if (gap > TOLERANCE_MM) {
                return false;
            }
            along = -(ra + gap / 2.0);
        }
    } else {
        const double a = (ra * ra - rb * rb + d * d) / (2.0 * d);
        double h2 = ra * ra - a * a;
        if (h2 < 0.0) {
            h2 = 0.0;
        }
        const double h = sqrt(h2);
        const double mx = ax + a * ux;
        const double my = ay + a * uy;
        *p1x = mx - uy * h;
        *p1y = my + ux * h;
        *p2x = mx + uy * h;
        *p2y = my - ux * h;
        return true;
    }

    *p1x = ax + along * ux;
    *p1y = ay + along * uy;
    *p2x = *p1x;
    *p2y = *p1y;
    return true;
}

typedef struct {
    size_t index;
    double horizontal_mm;
} reference_t;

static size_t solved_count(const nrf_geometry_solution_t *solution)
{
    size_t count = 0U;
    for (size_t i = 0; i < solution->node_count; ++i) {
        if (solution->nodes[i].solved) {
            ++count;
        }
    }
    return count;
}

/* Largest horizontal disagreement of candidate (x, y) with the references,
 * skipping reference indices skip_a/skip_b (pass ref_count to skip none). */
static double max_reference_error(
    const nrf_geometry_solution_t *solution,
    const reference_t *refs,
    size_t ref_count,
    size_t skip_a,
    size_t skip_b,
    double x,
    double y)
{
    double worst = 0.0;
    for (size_t r = 0; r < ref_count; ++r) {
        if (r == skip_a || r == skip_b) {
            continue;
        }
        const nrf_geometry_node_position_t *ref = &solution->nodes[refs[r].index];
        const double error = fabs(
            hypot(x - (double)ref->x_mm, y - (double)ref->y_mm) - refs[r].horizontal_mm
        );
        if (error > worst) {
            worst = error;
        }
    }
    return worst;
}

/* Test measured reference distances, not rounded placements that can distort a flat triangle. */
static bool references_can_be_collinear(
    const nrf_geometry_solution_t *solution, const reference_t *refs, size_t ref_count,
    const nrf_cs_pair_result_t *pairs, size_t pair_count)
{
    if (ref_count != 3U) return false;
    double low[3], high[3];
    size_t side = 0U;
    for (size_t a = 0U; a < 3U; ++a) for (size_t b = a + 1U; b < 3U; ++b) {
        const nrf_geometry_node_position_t *na = &solution->nodes[refs[a].index];
        const nrf_geometry_node_position_t *nb = &solution->nodes[refs[b].index];
        double slant;
        if (!find_pair_distance(pairs, pair_count, na->node_id, nb->node_id, &slant)) return false;
        const double dz = fabs((double)na->z_mm - nb->z_mm);
        /* Integer millimetre inputs represent half-mm intervals, including before removing Z. */
        const double upper = slant + 0.5;
        const double lower = fmax(slant - 0.5, 0.0);
        if (upper < dz) return false;
        low[side] = sqrt(fmax(lower * lower - dz * dz, 0.0));
        high[side++] = sqrt(fmax(upper * upper - dz * dz, 0.0));
    }
    for (size_t longest = 0U; longest < 3U; ++longest) {
        const size_t a = (longest + 1U) % 3U, b = (longest + 2U) % 3U;
        if (high[longest] >= low[a] + low[b] && low[longest] <= high[a] + high[b]) return true;
    }
    return false;
}

/* Circle placement returns the worst reference error, or -1 for unresolved ambiguity. */
static double place_from_references(
    const nrf_geometry_solution_t *solution,
    const reference_t *refs,
    size_t ref_count,
    size_t already_solved,
    bool check_mirror_ambiguity,
    double *out_x,
    double *out_y)
{
    size_t best_a = 0U;
    size_t best_b = 1U;
    double best_separation = -1.0;
    for (size_t a = 0; a < ref_count; ++a) {
        for (size_t b = a + 1U; b < ref_count; ++b) {
            const nrf_geometry_node_position_t *na = &solution->nodes[refs[a].index];
            const nrf_geometry_node_position_t *nb = &solution->nodes[refs[b].index];
            const double separation = hypot(
                (double)na->x_mm - (double)nb->x_mm,
                (double)na->y_mm - (double)nb->y_mm
            );
            if (separation > best_separation) {
                best_separation = separation;
                best_a = a;
                best_b = b;
            }
        }
    }

    const nrf_geometry_node_position_t *ref_a = &solution->nodes[refs[best_a].index];
    const nrf_geometry_node_position_t *ref_b = &solution->nodes[refs[best_b].index];
    double p1x, p1y, p2x, p2y;
    if (!circle_intersect(
            (double)ref_a->x_mm, (double)ref_a->y_mm, refs[best_a].horizontal_mm,
            (double)ref_b->x_mm, (double)ref_b->y_mm, refs[best_b].horizontal_mm,
            &p1x, &p1y, &p2x, &p2y)) {
        return -1.0;
    }

    double x = p1x;
    double y = p1y;
    if (ref_count >= 3U) {
        const double e1 = max_reference_error(solution, refs, ref_count, best_a, best_b, p1x, p1y);
        const double e2 = max_reference_error(solution, refs, ref_count, best_a, best_b, p2x, p2y);
        if (check_mirror_ambiguity && e1 <= TOLERANCE_MM && e2 <= TOLERANCE_MM) {
            for (size_t i = 0U; i < solution->node_count; ++i) {
                if (!solution->nodes[i].solved) continue;
                bool reference = false;
                for (size_t r = 0U; r < ref_count; ++r) reference = reference || refs[r].index == i;
                if (reference) continue;
                const double d1 = hypot(p1x - solution->nodes[i].x_mm, p1y - solution->nodes[i].y_mm);
                const double d2 = hypot(p2x - solution->nodes[i].x_mm, p2y - solution->nodes[i].y_mm);
                /* Three collinear references can still leave an independent mirror relative to another node. */
                if (fabs(d1 - d2) > TOLERANCE_MM) return -1.0;
            }
        }
        if (e2 < e1) {
            x = p2x;
            y = p2y;
        }
    } else if (hypot(p1x - p2x, p1y - p2y) > TOLERANCE_MM && already_solved > 2U) {
        /* Two references only, the two mirror candidates differ by more than
         * the consistency tolerance, and the layout's reflection was already
         * fixed by an earlier placement: picking one would be a guess whose
         * possible error exceeds what the solver accepts everywhere else (and
         * later nodes would be placed relative to it). */
        return -1.0;
    }
    /* Otherwise (first two-reference placement) p1 -- the candidate on the
     * left of the ref_a -> ref_b baseline -- defines the layout's +Y. */

    *out_x = x;
    *out_y = y;
    return max_reference_error(solution, refs, ref_count, ref_count, ref_count, x, y);
}

/*
 * Places `target` using the horizontal distances already known to every
 * currently-solved node. See the header for when a node is deliberately
 * left unsolved instead of being placed by convention. The layout keeps
 * the "arbitrary rotation/reflection" caveat documented in the header --
 * this function does not and cannot resolve true compass heading from
 * distances alone.
 */
static bool solve_node_position(
    nrf_geometry_solution_t *solution,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const nrf_geometry_node_input_t *target,
    double *out_x_mm,
    double *out_y_mm)
{
    reference_t refs[NRF_GEOMETRY_MAX_NODES];
    size_t ref_count = 0;

    for (size_t i = 0; i < solution->node_count; ++i) {
        if (!solution->nodes[i].solved) {
            continue;
        }
        double slant_mm;
        if (!find_pair_distance(
                pairs, pair_count,
                target->node_id, solution->nodes[i].node_id,
                &slant_mm)) {
            continue;
        }
        const double z_diff_mm =
            (double)target->z_mm - (double)solution->nodes[i].z_mm;
        double horizontal_mm;
        if (!horizontal_distance_mm(slant_mm, z_diff_mm, &horizontal_mm)) {
            continue;
        }
        refs[ref_count].index = i;
        refs[ref_count].horizontal_mm = horizontal_mm;
        ++ref_count;
    }

    if (ref_count == 0) {
        return false;
    }

    const size_t already_solved = solved_count(solution);

    if (ref_count == 1) {
        /* Only the first node placed after MAIN may define the +X axis;
         * later, one distance leaves the position on an unresolved circle. */
        if (already_solved != 1U) {
            return false;
        }
        const nrf_geometry_node_position_t *ref0 = &solution->nodes[refs[0].index];
        *out_x_mm = (double)ref0->x_mm + refs[0].horizontal_mm;
        *out_y_mm = (double)ref0->y_mm;
        return true;
    }

    double x = 0.0;
    double y = 0.0;
    const bool check_mirror = ref_count < already_solved &&
        references_can_be_collinear(solution, refs, ref_count, pairs, pair_count);
    double error = place_from_references(solution, refs, ref_count, already_solved, check_mirror, &x, &y);

    if (!(error >= 0.0 && error <= TOLERANCE_MM)) {
        /* MAX-144 outlier policy: with four or more references a single bad
         * pair is tolerated. Rebuild the candidate without each reference in
         * turn (the outlier may be one of the baseline circles, in which
         * case the candidate itself is wrong and a CORRECT reference would
         * look worst) and keep the most self-consistent leave-one-out
         * placement. The full-mesh refinement and the residual report still
         * see -- and flag -- the dropped pair. */
        if (ref_count < 4U) {
            return false;
        }
        double best_error = -1.0;
        for (size_t drop = 0; drop < ref_count; ++drop) {
            reference_t remaining[NRF_GEOMETRY_MAX_NODES];
            size_t remaining_count = 0U;
            for (size_t r = 0; r < ref_count; ++r) {
                if (r != drop) {
                    remaining[remaining_count++] = refs[r];
                }
            }
            double cx = 0.0;
            double cy = 0.0;
            const double candidate_error =
                place_from_references(solution, remaining, remaining_count, already_solved, false, &cx, &cy);
            if (candidate_error >= 0.0 && (best_error < 0.0 || candidate_error < best_error)) {
                best_error = candidate_error;
                x = cx;
                y = cy;
            }
        }
        if (!(best_error >= 0.0 && best_error <= TOLERANCE_MM)) {
            return false;
        }
    }

    *out_x_mm = x;
    *out_y_mm = y;
    return true;
}

/* Fills residual_mm / max_residual_mm / inconsistent_pair_count from the
 * final layout, using every valid pair (not only the ones used to place). */
static void compute_residuals(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution)
{
    for (size_t p = 0; p < pair_count; ++p) {
        const nrf_cs_pair_result_t *pair = &pairs[p];
        if (pair->state != NRF_CS_PAIR_VALID) {
            continue;
        }
        nrf_geometry_node_position_t *a = NULL;
        nrf_geometry_node_position_t *b = NULL;
        for (size_t i = 0; i < solution->node_count; ++i) {
            if (solution->nodes[i].node_id == pair->node_a) {
                a = &solution->nodes[i];
            } else if (solution->nodes[i].node_id == pair->node_b) {
                b = &solution->nodes[i];
            }
        }
        if (a == NULL || b == NULL) {
            continue;
        }

        const double dz = (double)a->z_mm - (double)b->z_mm;
        if ((double)pair->distance_mm + TOLERANCE_MM < fabs(dz)) {
            ++solution->inconsistent_pair_count;
            continue;
        }
        if (!a->solved || !b->solved) {
            continue;
        }

        const double dx = (double)a->x_mm - (double)b->x_mm;
        const double dy = (double)a->y_mm - (double)b->y_mm;
        const double residual =
            fabs(sqrt(dx * dx + dy * dy + dz * dz) - (double)pair->distance_mm);
        const int32_t residual_mm =
            residual >= (double)INT32_MAX ? INT32_MAX : (int32_t)lround(residual);

        if (residual_mm > a->residual_mm) {
            a->residual_mm = residual_mm;
        }
        if (residual_mm > b->residual_mm) {
            b->residual_mm = residual_mm;
        }
        if (residual_mm > solution->max_residual_mm) {
            solution->max_residual_mm = residual_mm;
        }
        if (residual > TOLERANCE_MM) {
            ++solution->inconsistent_pair_count;
        }
    }
}

/*
 * ---- MAX-144: weighted, robust full-mesh refinement ------------------------
 *
 * The sequential placement above uses at most three references per node and
 * ignores every other measured distance. This pass refines the X/Y of every
 * solved node against ALL valid pairs between solved nodes at once:
 * weighted nonlinear least squares (Levenberg-Marquardt) with a Huber robust
 * loss, initialized from the sequential layout, which it also falls back to
 * whenever the refinement fails or does not lower the cost.
 *
 * Gauge: distances fix the layout only up to rotation/reflection/translation.
 * MAIN stays at the origin, the first node placed after MAIN keeps y == 0
 * (it defines +X), and the reflection chosen by the initializer is kept.
 * Heights are known inputs, not unknowns.
 */

#define REFINE_MAX_UNKNOWNS (2U * NRF_GEOMETRY_MAX_NODES)
#define REFINE_MAX_ITERATIONS 30U
#define REFINE_HUBER_K 1.5

typedef struct {
    size_t a;
    size_t b;
    double distance_mm;
    double sigma_mm;
} refine_edge_t;

static double pair_sigma_mm(const nrf_cs_pair_result_t *pair)
{
    const double spread = pair->spread_mm > 0 ? (double)pair->spread_mm : 0.0;
    return spread > (double)NRF_GEOMETRY_RANGE_SIGMA_FLOOR_MM
        ? spread
        : (double)NRF_GEOMETRY_RANGE_SIGMA_FLOOR_MM;
}

/* Gaussian elimination with partial pivoting on an n x n system; `matrix`
 * and `rhs` are destroyed. */
static bool solve_dense(size_t n, double matrix[REFINE_MAX_UNKNOWNS][REFINE_MAX_UNKNOWNS], double *rhs)
{
    /* Relative singularity threshold: entries are in 1/mm^2 and tiny. */
    double scale = 0.0;
    for (size_t r = 0; r < n; ++r) {
        for (size_t c = 0; c < n; ++c) {
            if (fabs(matrix[r][c]) > scale) {
                scale = fabs(matrix[r][c]);
            }
        }
    }
    const double threshold = scale * 1e-12;

    for (size_t pivot = 0; pivot < n; ++pivot) {
        size_t best = pivot;
        for (size_t row = pivot + 1U; row < n; ++row) {
            if (fabs(matrix[row][pivot]) > fabs(matrix[best][pivot])) {
                best = row;
            }
        }
        if (!(fabs(matrix[best][pivot]) > threshold) || !(scale > 0.0)) {
            return false;
        }
        if (best != pivot) {
            for (size_t col = 0; col < n; ++col) {
                const double t = matrix[pivot][col];
                matrix[pivot][col] = matrix[best][col];
                matrix[best][col] = t;
            }
            const double t = rhs[pivot];
            rhs[pivot] = rhs[best];
            rhs[best] = t;
        }
        for (size_t row = pivot + 1U; row < n; ++row) {
            const double factor = matrix[row][pivot] / matrix[pivot][pivot];
            for (size_t col = pivot; col < n; ++col) {
                matrix[row][col] -= factor * matrix[pivot][col];
            }
            rhs[row] -= factor * rhs[pivot];
        }
    }
    for (size_t k = n; k-- > 0U;) {
        double sum = rhs[k];
        for (size_t col = k + 1U; col < n; ++col) {
            sum -= matrix[k][col] * rhs[col];
        }
        rhs[k] = sum / matrix[k][k];
        if (!isfinite(rhs[k])) {
            return false;
        }
    }
    return true;
}

typedef struct {
    int x_var[NRF_GEOMETRY_MAX_NODES]; /* -1 = fixed */
    int y_var[NRF_GEOMETRY_MAX_NODES];
    size_t unknowns;
} refine_layout_t;

static void node_xy(
    const nrf_geometry_solution_t *solution,
    const refine_layout_t *layout,
    const double *params,
    size_t node,
    double *x,
    double *y)
{
    *x = layout->x_var[node] >= 0 ? params[layout->x_var[node]] : (double)solution->nodes[node].x_mm;
    *y = layout->y_var[node] >= 0 ? params[layout->y_var[node]] : (double)solution->nodes[node].y_mm;
}

/* Builds J^T W J and J^T W r at `params`; returns the robust cost. */
static double refine_normal_equations(
    const nrf_geometry_solution_t *solution,
    const refine_layout_t *layout,
    const refine_edge_t *edges,
    size_t edge_count,
    const double *params,
    double normal[REFINE_MAX_UNKNOWNS][REFINE_MAX_UNKNOWNS],
    double *gradient)
{
    double cost = 0.0;
    if (normal != NULL) {
        memset(normal, 0, sizeof(double) * REFINE_MAX_UNKNOWNS * REFINE_MAX_UNKNOWNS);
        memset(gradient, 0, sizeof(double) * REFINE_MAX_UNKNOWNS);
    }

    for (size_t e = 0; e < edge_count; ++e) {
        double ax, ay, bx, by;
        node_xy(solution, layout, params, edges[e].a, &ax, &ay);
        node_xy(solution, layout, params, edges[e].b, &bx, &by);
        const double dz = (double)solution->nodes[edges[e].a].z_mm - (double)solution->nodes[edges[e].b].z_mm;
        const double dx = ax - bx;
        const double dy = ay - by;
        const double modeled = sqrt(dx * dx + dy * dy + dz * dz);
        const double residual = modeled - edges[e].distance_mm;
        const double sigma = edges[e].sigma_mm;
        const double scaled = fabs(residual) / sigma;

        /* Huber: quadratic within k sigma, linear beyond (IRLS weight). */
        double robust_weight = 1.0;
        if (scaled > REFINE_HUBER_K) {
            robust_weight = REFINE_HUBER_K / scaled;
            cost += 2.0 * REFINE_HUBER_K * scaled - REFINE_HUBER_K * REFINE_HUBER_K;
        } else {
            cost += scaled * scaled;
        }

        if (normal == NULL || modeled < 1.0) {
            continue;
        }
        const double weight = robust_weight / (sigma * sigma);
        double jac[REFINE_MAX_UNKNOWNS];
        int idx[4];
        size_t count = 0U;
        const int vars[4] = {
            layout->x_var[edges[e].a], layout->y_var[edges[e].a],
            layout->x_var[edges[e].b], layout->y_var[edges[e].b],
        };
        const double derivs[4] = { dx / modeled, dy / modeled, -dx / modeled, -dy / modeled };
        for (size_t k = 0; k < 4U; ++k) {
            if (vars[k] >= 0) {
                idx[count] = vars[k];
                jac[count] = derivs[k];
                ++count;
            }
        }
        for (size_t r = 0; r < count; ++r) {
            gradient[idx[r]] += weight * jac[r] * residual;
            for (size_t c = 0; c < count; ++c) {
                normal[idx[r]][idx[c]] += weight * jac[r] * jac[c];
            }
        }
    }
    return cost;
}

static void refine_solution(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    int main_index,
    int axis_index,
    nrf_geometry_solution_t *solution)
{
    refine_layout_t layout;
    double params[REFINE_MAX_UNKNOWNS];
    refine_edge_t edges[NRF_CS_MAX_PAIRS];
    size_t edge_count = 0U;

    layout.unknowns = 0U;
    for (size_t i = 0; i < solution->node_count; ++i) {
        layout.x_var[i] = -1;
        layout.y_var[i] = -1;
        if (!solution->nodes[i].solved || (int)i == main_index) {
            continue;
        }
        layout.x_var[i] = (int)layout.unknowns;
        params[layout.unknowns++] = (double)solution->nodes[i].x_mm;
        if ((int)i != axis_index) {
            layout.y_var[i] = (int)layout.unknowns;
            params[layout.unknowns++] = (double)solution->nodes[i].y_mm;
        }
    }
    if (layout.unknowns == 0U) {
        return;
    }

    for (size_t p = 0; p < pair_count && edge_count < NRF_CS_MAX_PAIRS; ++p) {
        if (pairs[p].state != NRF_CS_PAIR_VALID) {
            continue;
        }
        size_t a = SIZE_MAX;
        size_t b = SIZE_MAX;
        for (size_t i = 0; i < solution->node_count; ++i) {
            if (!solution->nodes[i].solved) {
                continue;
            }
            if (solution->nodes[i].node_id == pairs[p].node_a) a = i;
            if (solution->nodes[i].node_id == pairs[p].node_b) b = i;
        }
        if (a == SIZE_MAX || b == SIZE_MAX) {
            continue;
        }
        const double dz = (double)solution->nodes[a].z_mm - (double)solution->nodes[b].z_mm;
        if ((double)pairs[p].distance_mm + TOLERANCE_MM < fabs(dz)) {
            continue; /* physically impossible pair: never a constraint */
        }
        edges[edge_count].a = a;
        edges[edge_count].b = b;
        edges[edge_count].distance_mm = (double)pairs[p].distance_mm;
        edges[edge_count].sigma_mm = pair_sigma_mm(&pairs[p]);
        ++edge_count;
    }
    if (edge_count == 0U) {
        return;
    }

    double normal[REFINE_MAX_UNKNOWNS][REFINE_MAX_UNKNOWNS];
    double gradient[REFINE_MAX_UNKNOWNS];
    double cost = refine_normal_equations(solution, &layout, edges, edge_count, params, normal, gradient);
    const double initial_cost = cost;
    double lambda = 1e-3;
    uint8_t iterations = 0U;

    for (uint8_t iteration = 0U; iteration < REFINE_MAX_ITERATIONS; ++iteration) {
        double system[REFINE_MAX_UNKNOWNS][REFINE_MAX_UNKNOWNS];
        double step[REFINE_MAX_UNKNOWNS];
        bool accepted = false;

        /* Absolute regularization floor for directions with no curvature
         * (e.g. a node's y on the axis line: the reflection-degenerate
         * direction), so they neither block the solve nor force heavy
         * damping onto every other direction. */
        double trace = 0.0;
        for (size_t r = 0; r < layout.unknowns; ++r) {
            trace += normal[r][r];
        }
        const double floor_term = 1e-6 * trace / (double)layout.unknowns;

        for (unsigned attempt = 0U; attempt < 8U && !accepted; ++attempt) {
            for (size_t r = 0; r < layout.unknowns; ++r) {
                for (size_t c = 0; c < layout.unknowns; ++c) {
                    system[r][c] = normal[r][c];
                }
                system[r][r] += lambda * normal[r][r] + floor_term;
                step[r] = -gradient[r];
            }
            if (!solve_dense(layout.unknowns, system, step)) {
                lambda *= 10.0;
                continue;
            }
            double trial[REFINE_MAX_UNKNOWNS];
            for (size_t k = 0; k < layout.unknowns; ++k) {
                trial[k] = params[k] + step[k];
            }
            const double trial_cost =
                refine_normal_equations(solution, &layout, edges, edge_count, trial, NULL, NULL);
            if (isfinite(trial_cost) && trial_cost < cost) {
                memcpy(params, trial, sizeof(double) * layout.unknowns);
                cost = trial_cost;
                lambda = lambda > 1e-9 ? lambda / 10.0 : lambda;
                accepted = true;
            } else {
                lambda *= 10.0;
            }
        }
        if (!accepted) {
            break;
        }
        iterations = (uint8_t)(iteration + 1U);

        double step_norm = 0.0;
        for (size_t k = 0; k < layout.unknowns; ++k) {
            step_norm += step[k] * step[k];
        }
        cost = refine_normal_equations(solution, &layout, edges, edge_count, params, normal, gradient);
        if (sqrt(step_norm) < 0.1) {
            break;
        }
    }

    if (!(cost <= initial_cost)) {
        return; /* keep the sequential layout */
    }
    for (size_t i = 0; i < solution->node_count; ++i) {
        double x, y;
        node_xy(solution, &layout, params, i, &x, &y);
        if (!rounded_position_fits_i32(x) || !rounded_position_fits_i32(y)) {
            return;
        }
    }
    for (size_t i = 0; i < solution->node_count; ++i) {
        double x, y;
        node_xy(solution, &layout, params, i, &x, &y);
        solution->nodes[i].x_mm = (int32_t)lround(x);
        solution->nodes[i].y_mm = (int32_t)lround(y);
    }
    solution->refined = true;
    solution->refine_iterations = iterations;

    /* Per-node 1-sigma horizontal uncertainty from the covariance
     * (J^T W J)^-1 at the refined layout (MAIN is the fixed origin: 0). */
    (void)refine_normal_equations(solution, &layout, edges, edge_count, params, normal, gradient);
    double covariance_trace = 0.0;
    for (size_t r = 0; r < layout.unknowns; ++r) {
        covariance_trace += normal[r][r];
    }
    /* Same tiny floor as the solve: a direction the mesh does not constrain
     * then shows up as a huge variance for THAT node only, instead of making
     * the whole matrix singular for every node. */
    const double covariance_floor = 1e-9 * covariance_trace / (double)layout.unknowns;
    for (size_t i = 0; i < solution->node_count; ++i) {
        if (layout.x_var[i] < 0) {
            continue;
        }
        double variance = 0.0;
        bool ok = true;
        const int vars[2] = { layout.x_var[i], layout.y_var[i] };
        for (size_t v = 0; v < 2U && ok; ++v) {
            if (vars[v] < 0) {
                continue;
            }
            double system[REFINE_MAX_UNKNOWNS][REFINE_MAX_UNKNOWNS];
            double column[REFINE_MAX_UNKNOWNS];
            for (size_t r = 0; r < layout.unknowns; ++r) {
                for (size_t c = 0; c < layout.unknowns; ++c) {
                    system[r][c] = normal[r][c];
                }
                system[r][r] += covariance_floor;
                column[r] = (int)r == vars[v] ? 1.0 : 0.0;
            }
            ok = solve_dense(layout.unknowns, system, column);
            if (ok) {
                variance += column[vars[v]];
            }
        }
        if (ok && isfinite(variance) && variance >= 0.0) {
            const double sigma = sqrt(variance);
            solution->nodes[i].uncertainty_mm =
                sigma >= (double)INT32_MAX ? INT32_MAX : (int32_t)lround(sigma);
        } else {
            /* Singular/ill-conditioned: this node's position is not
             * determined by the mesh -- report it as such. */
            solution->nodes[i].uncertainty_mm = INT32_MAX;
        }
    }
}

/*
 * MAX-144: two unsolved nodes that each have exactly two references are a
 * mirror ambiguity one at a time, but a measured pair BETWEEN them can tell
 * the four mirror combinations apart. Places both when exactly one
 * combination agrees with that pair within the tolerance; never guesses.
 */
static bool two_reference_candidates(
    const nrf_geometry_solution_t *solution,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const nrf_geometry_node_input_t *target,
    double candidates[2][2])
{
    reference_t refs[2];
    size_t ref_count = 0U;
    for (size_t i = 0; i < solution->node_count; ++i) {
        if (!solution->nodes[i].solved) {
            continue;
        }
        double slant_mm;
        double horizontal_mm;
        if (!find_pair_distance(pairs, pair_count, target->node_id, solution->nodes[i].node_id, &slant_mm) ||
            !horizontal_distance_mm(slant_mm, (double)target->z_mm - (double)solution->nodes[i].z_mm, &horizontal_mm)) {
            continue;
        }
        if (ref_count == 2U) {
            return false; /* three or more references: not this case */
        }
        refs[ref_count].index = i;
        refs[ref_count].horizontal_mm = horizontal_mm;
        ++ref_count;
    }
    if (ref_count != 2U) {
        return false;
    }
    const nrf_geometry_node_position_t *a = &solution->nodes[refs[0].index];
    const nrf_geometry_node_position_t *b = &solution->nodes[refs[1].index];
    return circle_intersect(
        (double)a->x_mm, (double)a->y_mm, refs[0].horizontal_mm,
        (double)b->x_mm, (double)b->y_mm, refs[1].horizontal_mm,
        &candidates[0][0], &candidates[0][1], &candidates[1][0], &candidates[1][1]);
}

static const nrf_geometry_node_input_t *input_for_node(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    nrf_cs_node_id_t node_id)
{
    for (size_t j = 0; j < input_count; ++j) {
        if (inputs[j].node_id == node_id) {
            return &inputs[j];
        }
    }
    return NULL;
}

static bool resolve_ambiguous_pair(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution)
{
    for (size_t i = 0; i < solution->node_count; ++i) {
        const nrf_geometry_node_input_t *ti = input_for_node(inputs, input_count, solution->nodes[i].node_id);
        double ci[2][2];
        if (solution->nodes[i].solved || ti == NULL ||
            !two_reference_candidates(solution, pairs, pair_count, ti, ci)) {
            continue;
        }
        for (size_t j = i + 1U; j < solution->node_count; ++j) {
            const nrf_geometry_node_input_t *tj = input_for_node(inputs, input_count, solution->nodes[j].node_id);
            double cj[2][2];
            double slant_mm;
            double horizontal_mm;
            if (solution->nodes[j].solved || tj == NULL ||
                !two_reference_candidates(solution, pairs, pair_count, tj, cj) ||
                !find_pair_distance(pairs, pair_count, ti->node_id, tj->node_id, &slant_mm) ||
                !horizontal_distance_mm(slant_mm, (double)ti->z_mm - (double)tj->z_mm, &horizontal_mm)) {
                continue;
            }
            double best = -1.0;
            double second = -1.0;
            size_t best_a = 0U;
            size_t best_b = 0U;
            for (size_t a = 0; a < 2U; ++a) {
                for (size_t b = 0; b < 2U; ++b) {
                    const double error = fabs(
                        hypot(ci[a][0] - cj[b][0], ci[a][1] - cj[b][1]) - horizontal_mm);
                    if (best < 0.0 || error < best) {
                        second = best;
                        best = error;
                        best_a = a;
                        best_b = b;
                    } else if (second < 0.0 || error < second) {
                        second = error;
                    }
                }
            }
            if (!(best <= TOLERANCE_MM && second > TOLERANCE_MM)) {
                continue;
            }
            if (!rounded_position_fits_i32(ci[best_a][0]) || !rounded_position_fits_i32(ci[best_a][1]) ||
                !rounded_position_fits_i32(cj[best_b][0]) || !rounded_position_fits_i32(cj[best_b][1])) {
                continue;
            }
            solution->nodes[i].x_mm = (int32_t)lround(ci[best_a][0]);
            solution->nodes[i].y_mm = (int32_t)lround(ci[best_a][1]);
            solution->nodes[i].solved = true;
            solution->nodes[j].x_mm = (int32_t)lround(cj[best_b][0]);
            solution->nodes[j].y_mm = (int32_t)lround(cj[best_b][1]);
            solution->nodes[j].solved = true;
            return true;
        }
    }
    return false;
}

static nrf_geometry_result_t nrf_geometry_solver_solve_impl(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution,
    bool enforce_recording_minimum,
    nrf_cs_node_id_t anchor_node_id)
{
    if (inputs == NULL || solution == NULL ||
        input_count == 0 || input_count > NRF_GEOMETRY_MAX_NODES ||
        (pairs == NULL && pair_count != 0)) {
        return NRF_GEOMETRY_ERR_ARGUMENT;
    }

    for (size_t i = 0u; i < input_count; ++i) {
        for (size_t j = i + 1u; j < input_count; ++j) {
            if (inputs[i].node_id == inputs[j].node_id) {
                return NRF_GEOMETRY_ERR_ARGUMENT;
            }
        }
    }

    for (size_t i = 0u; i < pair_count; ++i) {
        if (
            pairs[i].state == NRF_CS_PAIR_VALID &&
            (pairs[i].distance_mm < 0 || pairs[i].node_a == pairs[i].node_b)
        ) {
            return NRF_GEOMETRY_ERR_ARGUMENT;
        }
    }

    memset(solution, 0, sizeof(*solution));
    solution->node_count = input_count;
    solution->anchor_node_id = anchor_node_id;

    int main_index = -1; /* index of the anchor: MAIN (id 0) for the legacy calls */
    for (size_t i = 0; i < input_count; ++i) {
        solution->nodes[i].node_id = inputs[i].node_id;
        solution->nodes[i].z_mm = inputs[i].z_mm;
        solution->nodes[i].solved = false;
        solution->nodes[i].x_mm = 0;
        solution->nodes[i].y_mm = 0;
        if (inputs[i].node_id == anchor_node_id) {
            if (main_index >= 0) {
                return NRF_GEOMETRY_ERR_ARGUMENT;
            }
            main_index = (int)i;
        }
    }
    if (main_index < 0) {
        return NRF_GEOMETRY_ERR_NO_MAIN;
    }

    solution->nodes[main_index].solved = true;
    solution->nodes[main_index].x_mm = 0;
    solution->nodes[main_index].y_mm = 0;

    if (enforce_recording_minimum &&
        input_count < 1U + NRF_GEOMETRY_MIN_REMOTE_COUNT_FOR_RECORDING) {
        return NRF_GEOMETRY_UNS_INSUFFICIENT_NODES;
    }

    /* Iteratively place every node reachable through valid CS pairs from
     * the already-solved set. Nodes with no such path stay unsolved. The
     * first node placed after MAIN defines +X (the refinement gauge). */
    size_t first_index = 0U;
    for (size_t i = 0U; i < solution->node_count; ++i) {
        double slant_mm, horizontal_mm;
        if ((int)i != main_index && find_pair_distance(pairs, pair_count,
                solution->nodes[main_index].node_id, solution->nodes[i].node_id, &slant_mm) &&
            horizontal_distance_mm(slant_mm,
                (double)solution->nodes[i].z_mm - solution->nodes[main_index].z_mm, &horizontal_mm) &&
            horizontal_mm >= 1.0) {
            /* A coincident horizontal projection cannot define the rotation gauge. */
            first_index = i;
            break;
        }
    }
    int axis_index = -1;
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t offset = 0U; offset < solution->node_count; ++offset) {
            const size_t i = (first_index + offset) % solution->node_count;
            if (solution->nodes[i].solved) {
                continue;
            }
            const nrf_geometry_node_input_t *target = NULL;
            for (size_t j = 0; j < input_count; ++j) {
                if (inputs[j].node_id == solution->nodes[i].node_id) {
                    target = &inputs[j];
                    break;
                }
            }
            if (target == NULL) {
                continue;
            }
            double x_mm, y_mm;
            const bool defines_axis = solved_count(solution) == 1U;
            if (solve_node_position(solution, pairs, pair_count, target, &x_mm, &y_mm)) {
                if (!rounded_position_fits_i32(x_mm) || !rounded_position_fits_i32(y_mm)) {
                    continue;
                }
                solution->nodes[i].x_mm = (int32_t)lround(x_mm);
                solution->nodes[i].y_mm = (int32_t)lround(y_mm);
                solution->nodes[i].solved = true;
                if (defines_axis && axis_index < 0) {
                    axis_index = (int)i;
                }
                progress = true;
            }
        }
        if (!progress) {
            progress = resolve_ambiguous_pair(inputs, input_count, pairs, pair_count, solution);
        }
    }

    refine_solution(pairs, pair_count, main_index, axis_index, solution);
    compute_residuals(pairs, pair_count, solution);
    return NRF_GEOMETRY_OK;
}


nrf_geometry_result_t nrf_geometry_solver_solve(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution)
{
    return nrf_geometry_solver_solve_impl(
        inputs, input_count, pairs, pair_count, solution, true, 0U
    );
}


nrf_geometry_result_t nrf_geometry_solver_solve_partial(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution)
{
    return nrf_geometry_solver_solve_impl(
        inputs, input_count, pairs, pair_count, solution, false, 0U
    );
}


/* The legacy "no node 0" answer is NO_MAIN; with an explicit anchor the same condition is NO_ANCHOR. */
static nrf_geometry_result_t anchored_result(nrf_geometry_result_t result)
{
    return result == NRF_GEOMETRY_ERR_NO_MAIN ? NRF_GEOMETRY_ERR_NO_ANCHOR : result;
}

nrf_geometry_result_t nrf_geometry_solver_solve_with_anchor(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_cs_node_id_t anchor_node_id,
    nrf_geometry_solution_t *solution)
{
    return anchored_result(nrf_geometry_solver_solve_impl(
        inputs, input_count, pairs, pair_count, solution, true, anchor_node_id
    ));
}


nrf_geometry_result_t nrf_geometry_solver_solve_partial_with_anchor(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_cs_node_id_t anchor_node_id,
    nrf_geometry_solution_t *solution)
{
    return anchored_result(nrf_geometry_solver_solve_impl(
        inputs, input_count, pairs, pair_count, solution, false, anchor_node_id
    ));
}
