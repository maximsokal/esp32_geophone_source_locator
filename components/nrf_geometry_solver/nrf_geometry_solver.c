#include "nrf_geometry_solver.h"

#include <math.h>
#include <string.h>

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

/*
 * Horizontal (X/Y-plane) distance from a known slant distance and the
 * already-known vertical separation. Clamped to zero if noise pushes the
 * radicand slightly negative rather than producing NaN.
 */
static double horizontal_distance_mm(double slant_mm, double z_diff_mm)
{
    const double radicand = slant_mm * slant_mm - z_diff_mm * z_diff_mm;
    if (radicand <= 0.0) {
        return 0.0;
    }
    return sqrt(radicand);
}

/*
 * Two circles of radius ra/rb centered at (ax,ay)/(bx,by) intersect in at
 * most two points; returns false if the circles are too far apart, one
 * inside the other, or coincident (degenerate/no unique solution).
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

    if (d < 1e-6 || d > ra + rb || d < fabs(ra - rb)) {
        return false;
    }

    const double a = (ra * ra - rb * rb + d * d) / (2.0 * d);
    double h2 = ra * ra - a * a;
    if (h2 < 0.0) {
        h2 = 0.0;
    }
    const double h = sqrt(h2);
    const double mx = ax + a * dx / d;
    const double my = ay + a * dy / d;
    const double ox = -dy / d * h;
    const double oy = dx / d * h;

    *p1x = mx + ox;
    *p1y = my + oy;
    *p2x = mx - ox;
    *p2y = my - oy;
    return true;
}

/*
 * Places `target` using the horizontal distances already known to every
 * currently-solved node. One reference: arbitrary heading, places along
 * +X from that reference. Two or more references: circle-intersection,
 * disambiguating the mirror choice against a third reference when one is
 * available; otherwise keeps the non-negative-Y candidate by convention.
 * Both choices carry the same "arbitrary rotation/reflection" caveat
 * documented in the header -- this function does not and cannot resolve
 * true compass heading from distances alone.
 */
static bool solve_node_position(
    nrf_geometry_solution_t *solution,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const nrf_geometry_node_input_t *target,
    double *out_x_mm,
    double *out_y_mm)
{
    int ref_index[NRF_GEOMETRY_MAX_NODES];
    double ref_horizontal_mm[NRF_GEOMETRY_MAX_NODES];
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
        ref_index[ref_count] = (int)i;
        ref_horizontal_mm[ref_count] = horizontal_distance_mm(slant_mm, z_diff_mm);
        ++ref_count;
    }

    if (ref_count == 0) {
        return false;
    }

    const nrf_geometry_node_position_t *ref0 = &solution->nodes[ref_index[0]];
    if (ref_count == 1) {
        *out_x_mm = (double)ref0->x_mm + ref_horizontal_mm[0];
        *out_y_mm = (double)ref0->y_mm;
        return true;
    }

    const nrf_geometry_node_position_t *ref1 = &solution->nodes[ref_index[1]];
    double p1x, p1y, p2x, p2y;
    if (!circle_intersect(
            (double)ref0->x_mm, (double)ref0->y_mm, ref_horizontal_mm[0],
            (double)ref1->x_mm, (double)ref1->y_mm, ref_horizontal_mm[1],
            &p1x, &p1y, &p2x, &p2y)) {
        /* Degenerate pair geometry (e.g. references coincide): fall back
         * to the single-reference placement using the first reference. */
        *out_x_mm = (double)ref0->x_mm + ref_horizontal_mm[0];
        *out_y_mm = (double)ref0->y_mm;
        return true;
    }

    if (ref_count >= 3) {
        const nrf_geometry_node_position_t *ref2 = &solution->nodes[ref_index[2]];
        const double expected_mm = ref_horizontal_mm[2];
        const double d1 = fabs(hypot(p1x - (double)ref2->x_mm, p1y - (double)ref2->y_mm) - expected_mm);
        const double d2 = fabs(hypot(p2x - (double)ref2->x_mm, p2y - (double)ref2->y_mm) - expected_mm);
        if (d2 < d1) {
            *out_x_mm = p2x;
            *out_y_mm = p2y;
            return true;
        }
    }

    *out_x_mm = p1x;
    *out_y_mm = p1y;
    return true;
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
                solution->nodes[i].x_mm = (int32_t)lround(x_mm);
                solution->nodes[i].y_mm = (int32_t)lround(y_mm);
                solution->nodes[i].solved = true;
                progress = true;
            }
        }
    }

    return NRF_GEOMETRY_OK;
}
