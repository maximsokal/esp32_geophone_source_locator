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

    /* Most widely separated reference pair: the best-conditioned baseline. */
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
        return false;
    }

    const double mirror_separation = hypot(p1x - p2x, p1y - p2y);
    double x = p1x;
    double y = p1y;

    if (ref_count >= 3U) {
        const double e1 = max_reference_error(solution, refs, ref_count, best_a, best_b, p1x, p1y);
        const double e2 = max_reference_error(solution, refs, ref_count, best_a, best_b, p2x, p2y);
        if (e2 < e1) {
            x = p2x;
            y = p2y;
        }
    } else if (mirror_separation > 2.0 * TOLERANCE_MM && already_solved > 2U) {
        /* Two references only, the two mirror candidates are clearly
         * distinct, and the layout's reflection was already fixed by an
         * earlier placement: picking one would be a guess. */
        return false;
    }
    /* Otherwise (first two-reference placement) p1 -- the candidate on the
     * left of the ref_a -> ref_b baseline -- defines the layout's +Y. */

    if (max_reference_error(solution, refs, ref_count, ref_count, ref_count, x, y) > TOLERANCE_MM) {
        return false;
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

nrf_geometry_result_t nrf_geometry_solver_solve(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution)
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

    int main_index = -1;
    for (size_t i = 0; i < input_count; ++i) {
        solution->nodes[i].node_id = inputs[i].node_id;
        solution->nodes[i].z_mm = inputs[i].z_mm;
        solution->nodes[i].solved = false;
        solution->nodes[i].x_mm = 0;
        solution->nodes[i].y_mm = 0;
        if (inputs[i].node_id == 0U) {
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

    /* Iteratively place every node reachable through valid CS pairs from
     * the already-solved set. Nodes with no such path stay unsolved. */
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < solution->node_count; ++i) {
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
            if (solve_node_position(solution, pairs, pair_count, target, &x_mm, &y_mm)) {
                if (!rounded_position_fits_i32(x_mm) || !rounded_position_fits_i32(y_mm)) {
                    continue;
                }
                solution->nodes[i].x_mm = (int32_t)lround(x_mm);
                solution->nodes[i].y_mm = (int32_t)lround(y_mm);
                solution->nodes[i].solved = true;
                progress = true;
            }
        }
    }

    compute_residuals(pairs, pair_count, solution);
    return NRF_GEOMETRY_OK;
}
