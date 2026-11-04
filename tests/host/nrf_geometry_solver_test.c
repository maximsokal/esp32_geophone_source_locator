#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "nrf_geometry_solver.h"

static nrf_cs_pair_result_t make_pair(
    nrf_cs_node_id_t a,
    nrf_cs_node_id_t b,
    nrf_cs_pair_state_t state,
    int32_t distance_mm)
{
    nrf_cs_pair_result_t pair = {0};
    pair.node_a = a;
    pair.node_b = b;
    pair.state = state;
    pair.distance_mm = distance_mm;
    return pair;
}

static const nrf_geometry_node_position_t *find_node(
    const nrf_geometry_solution_t *solution,
    nrf_cs_node_id_t node_id)
{
    for (size_t i = 0; i < solution->node_count; ++i) {
        if (solution->nodes[i].node_id == node_id) {
            return &solution->nodes[i];
        }
    }
    return NULL;
}

static void test_rejects_missing_main_and_bad_arguments(void)
{
    nrf_geometry_node_input_t inputs[1] = {{.node_id = 1U, .z_mm = 0}};
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve(inputs, 1U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_ERR_NO_MAIN);
    assert(nrf_geometry_solver_solve(NULL, 1U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve(inputs, 0U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_ERR_ARGUMENT);
}

static void test_main_alone_is_the_origin(void)
{
    nrf_geometry_node_input_t inputs[1] = {{.node_id = 0U, .z_mm = 0}};
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 1U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_OK);
    assert(solution.node_count == 1U);
    const nrf_geometry_node_position_t *main_node = find_node(&solution, 0U);
    assert(main_node != NULL);
    assert(main_node->solved);
    assert(main_node->x_mm == 0 && main_node->y_mm == 0);
}


static void test_less_than_three_remote_nodes_return_unsolved(void)
{
    nrf_geometry_node_input_t inputs[3] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[3] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 300),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 300),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 400),
    };

    for (size_t input_count = 1U; input_count <= 3U; ++input_count) {
        const size_t pair_count = input_count == 1U ? 0U :
            input_count == 2U ? 1U : 3U;
        nrf_geometry_solution_t solution;
        assert(nrf_geometry_solver_solve(
            inputs, input_count, pair_count == 0U ? NULL : pairs,
            pair_count, &solution
        ) == NRF_GEOMETRY_UNS_INSUFFICIENT_NODES);
        assert(solution.node_count == input_count);
        const nrf_geometry_node_position_t *main_node = find_node(&solution, 0U);
        assert(main_node != NULL && main_node->solved);
        for (size_t node = 1U; node < input_count; ++node) {
            const nrf_geometry_node_position_t *remote =
                find_node(&solution, (nrf_cs_node_id_t)node);
            assert(remote != NULL && !remote->solved);
            assert(remote->x_mm == 0 && remote->y_mm == 0);
        }
    }
}

static void test_single_reference_places_along_arbitrary_heading(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[1] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 300),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && a->solved);
    assert(a->x_mm == 300 && a->y_mm == 0);
}

static void test_isolated_node_with_no_valid_pair_stays_unsolved(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
    };
    nrf_geometry_solution_t solution;

    /* No pairs at all. */
    assert(nrf_geometry_solver_solve_partial(inputs, 2U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && !a->solved);
}

static void test_missing_pair_state_is_not_treated_as_zero_distance(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
    };
    /* distance_mm left at 0 to simulate exactly the failure this must not
     * be confused with: a MISSING pair with a zeroed measurement field. */
    nrf_cs_pair_result_t pairs[1] = {
        make_pair(0U, 1U, NRF_CS_PAIR_MISSING, 0),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && !a->solved);
}

static void test_rejects_negative_valid_distance_and_duplicate_node_ids(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
    };
    nrf_cs_pair_result_t negative_pair =
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, -1);
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve(
        inputs, 2U, &negative_pair, 1U, &solution
    ) == NRF_GEOMETRY_ERR_ARGUMENT);

    negative_pair.node_a = 1U;
    negative_pair.node_b = 1U;
    negative_pair.distance_mm = 10;
    assert(nrf_geometry_solver_solve(
        inputs, 2U, &negative_pair, 1U, &solution
    ) == NRF_GEOMETRY_ERR_ARGUMENT);

    inputs[1].node_id = 0U;
    assert(nrf_geometry_solver_solve(
        inputs, 2U, NULL, 0U, &solution
    ) == NRF_GEOMETRY_ERR_ARGUMENT);
}

static void test_overflowing_position_is_not_marked_solved(void)
{
    nrf_geometry_node_input_t inputs[3] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[2] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, INT32_MAX),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, INT32_MAX),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 3U, pairs, 2U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *first = find_node(&solution, 1U);
    const nrf_geometry_node_position_t *overflowing = find_node(&solution, 2U);
    assert(first != NULL && first->solved && first->x_mm == INT32_MAX);
    assert(overflowing != NULL && !overflowing->solved);
}

static int32_t iabs(int32_t value)
{
    return value < 0 ? -value : value;
}

typedef struct
{
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
} test_point_t;

static int32_t point_distance(const test_point_t *a, const test_point_t *b)
{
    const double dx = (double)a->x_mm - (double)b->x_mm;
    const double dy = (double)a->y_mm - (double)b->y_mm;
    const double dz = (double)a->z_mm - (double)b->z_mm;
    return (int32_t)lround(sqrt(dx * dx + dy * dy + dz * dz));
}

static int32_t deterministic_noise(uint32_t *state, int32_t amplitude)
{
    *state = *state * 1664525U + 1013904223U;
    const uint32_t span = (uint32_t)(amplitude * 2 + 1);
    return (int32_t)(*state % span) - amplitude;
}

static void test_deterministic_randomized_noise_and_node_order(void)
{
    /* Fixed-seed pseudo-random perturbations keep this property test
     * reproducible while exercising the non-ideal ranges from the audit. */
    static const test_point_t points[5] = {
        {0, 0, 0},
        {1350, 240, 180},
        {-620, 1130, -140},
        {780, -880, 390},
        {-1040, -430, 70},
    };
    static const nrf_cs_node_id_t pair_a[10] = {0U, 0U, 0U, 0U, 1U, 1U, 1U, 2U, 2U, 3U};
    static const nrf_cs_node_id_t pair_b[10] = {1U, 2U, 3U, 4U, 2U, 3U, 4U, 3U, 4U, 4U};
    static const nrf_cs_node_id_t input_order[5] = {0U, 3U, 1U, 4U, 2U};
    static const int32_t amplitudes[] = {0, 10, 50, 100, 300};

    for (size_t sweep = 0; sweep < sizeof(amplitudes) / sizeof(amplitudes[0]); ++sweep) {
        const int32_t amplitude = amplitudes[sweep];
        nrf_geometry_node_input_t inputs[5] = {0};
        for (size_t i = 0; i < 5U; ++i) {
            const nrf_cs_node_id_t node_id = input_order[i];
            inputs[i].node_id = node_id;
            inputs[i].z_mm = points[node_id].z_mm;
        }

        nrf_cs_pair_result_t pairs[10] = {0};
        uint32_t random_state = 0x51A7E5U + (uint32_t)sweep;
        for (size_t i = 0; i < 10U; ++i) {
            pairs[i] = make_pair(
                pair_a[i],
                pair_b[i],
                NRF_CS_PAIR_VALID,
                point_distance(&points[pair_a[i]], &points[pair_b[i]]) +
                    deterministic_noise(&random_state, amplitude)
            );
        }
        /* A missing edge must not make the solver manufacture a zero range;
         * the remaining MAIN-connected mesh is still sufficient. */
        if (sweep == 4U) pairs[9].state = NRF_CS_PAIR_MISSING;

        nrf_geometry_solution_t solution;
        assert(nrf_geometry_solver_solve(inputs, 5U, pairs, 10U, &solution) == NRF_GEOMETRY_OK);
        assert(solution.node_count == 5U);

        for (size_t i = 0; i < 5U; ++i) {
            const nrf_geometry_node_position_t *node = find_node(&solution, input_order[i]);
            assert(node != NULL && node->solved);
            assert(node->z_mm == points[node->node_id].z_mm);
            assert(iabs(node->x_mm) < 10000 && iabs(node->y_mm) < 10000);
        }

        if (amplitude == 0) {
            for (size_t i = 0; i < 10U; ++i) {
                const nrf_geometry_node_position_t *a = find_node(&solution, pair_a[i]);
                const nrf_geometry_node_position_t *b = find_node(&solution, pair_b[i]);
                const int32_t dx = a->x_mm - b->x_mm;
                const int32_t dy = a->y_mm - b->y_mm;
                const int32_t dz = a->z_mm - b->z_mm;
                const int32_t solved_distance = (int32_t)lround(
                    sqrt((double)dx * dx + (double)dy * dy + (double)dz * dz)
                );
                assert(iabs(solved_distance - pairs[i].distance_mm) <= 8);
            }
        }
    }
}

static void test_trilateration_with_mirror_disambiguation(void)
{
    /*
     * MAIN=(0,0), A=(300,0) via a single reference. B=(150,200) and
     * C=(150,-200) are exact mirror images of each other across the
     * MAIN-A axis and have identical distances to both MAIN (250) and A
     * (250) -- distinguishable only via a third reference. C's pair to B
     * (400mm, their true separation) is what the solver must use to pick
     * the correct mirror.
     */
    nrf_geometry_node_input_t inputs[4] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
        {.node_id = 3U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[6] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 300),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 250),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 250),
        make_pair(0U, 3U, NRF_CS_PAIR_VALID, 250),
        make_pair(1U, 3U, NRF_CS_PAIR_VALID, 250),
        make_pair(2U, 3U, NRF_CS_PAIR_VALID, 400),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, 6U, &solution) ==
           NRF_GEOMETRY_OK);

    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    const nrf_geometry_node_position_t *b = find_node(&solution, 2U);
    const nrf_geometry_node_position_t *c = find_node(&solution, 3U);
    assert(a != NULL && a->solved);
    assert(b != NULL && b->solved);
    assert(c != NULL && c->solved);

    assert(a->x_mm == 300 && a->y_mm == 0);
    assert(iabs(b->x_mm - 150) <= 2 && iabs(b->y_mm - 200) <= 2);
    /* The mirror-disambiguated point, not (150,200) again. */
    assert(iabs(c->x_mm - 150) <= 2 && iabs(c->y_mm - (-200)) <= 2);
}

static void test_z_passes_through_unmodified(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 400},
    };
    nrf_cs_pair_result_t pairs[1] = {
        /* Exact 3-4-5 triangle (x100): 300mm horizontal, 400mm vertical,
         * 500mm slant -- no rounding error to budget for. */
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 500),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && a->solved);
    assert(a->z_mm == 400);
    assert(iabs(a->x_mm - 300) <= 1);
}

/* MAX-205: a slant distance shorter than the height difference is physically
 * impossible; it must not collapse to a zero horizontal distance and be
 * reported as a solved position on top of MAIN. */
static void test_impossible_height_difference_is_not_solved(void)
{
    nrf_geometry_node_input_t inputs[2] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 110000},
    };
    nrf_cs_pair_result_t pairs[1] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 5000),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && !a->solved);
    assert(solution.inconsistent_pair_count == 1U);
}

/* Circles that miss by far more than range noise must not fall back to an
 * arbitrary +X placement. MAIN=(0,0), A=(4000,0); B claims 1000 mm to both. */
static void test_disjoint_reference_circles_leave_node_unsolved(void)
{
    nrf_geometry_node_input_t inputs[3] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[3] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 4000),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 1000),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 1000),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 3U, pairs, 3U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *b = find_node(&solution, 2U);
    assert(b != NULL && !b->solved);
}

/* A small miss (range noise) is resolved, with the disagreement reported as
 * a residual rather than hidden. Sequential placement alone puts B at the
 * closest-approach midpoint (2000,0) with the whole 200 mm miss on B's two
 * pairs (100 mm each); the MAX-144 full-mesh refinement also moves A and
 * spreads the triangle inequality violation over all three equally-weighted
 * pairs (~67 mm each). */
static void test_near_miss_within_tolerance_is_solved_with_residual(void)
{
    nrf_geometry_node_input_t inputs[3] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[3] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 4000),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 1900),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 1900),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 3U, pairs, 3U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *b = find_node(&solution, 2U);
    assert(b != NULL && b->solved);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(solution.refined);
    assert(iabs(b->y_mm) <= 1);
    assert(a->x_mm < 4000 && a->y_mm == 0);
    assert(iabs(b->x_mm * 2 - a->x_mm) <= 2);   /* B stays midway */
    assert(solution.max_residual_mm >= 60 && solution.max_residual_mm <= 72);
    assert(solution.inconsistent_pair_count == 0U);
}

/* Only the first node after MAIN may be placed from a single distance; a
 * later one would be an arbitrary point on a circle. */
static void test_second_single_reference_node_stays_unsolved(void)
{
    nrf_geometry_node_input_t inputs[3] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[2] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 3000),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 3000),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve_partial(inputs, 3U, pairs, 2U, &solution) ==
           NRF_GEOMETRY_OK);
    assert(find_node(&solution, 1U)->solved);
    assert(!find_node(&solution, 2U)->solved);
}

/* Once the layout's reflection is fixed, two references that leave two
 * clearly distinct mirror candidates must not be resolved by guessing.
 * MAIN=(0,0), A=(3000,0), B=(0,3000); C pairs only with A and B. */
static void test_two_reference_mirror_ambiguity_after_reflection_fixed(void)
{
    nrf_geometry_node_input_t inputs[4] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
        {.node_id = 3U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[5] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 3000),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 3000),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 4243),
        /* C truly at (3000,3000): 3000 to A and to B; mirror is (0,0). */
        make_pair(1U, 3U, NRF_CS_PAIR_VALID, 3000),
        make_pair(2U, 3U, NRF_CS_PAIR_VALID, 3000),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, 5U, &solution) ==
           NRF_GEOMETRY_OK);
    assert(find_node(&solution, 1U)->solved);
    assert(find_node(&solution, 2U)->solved);
    assert(!find_node(&solution, 3U)->solved);
}

/* A reference that disagrees with the two-circle placement by more than the
 * tolerance makes the node unsolved instead of silently accepted.
 * MAIN=(0,0), A=(3000,0), B=(0,4000); C truly at (3000,4000) but its pair
 * to MAIN claims 3200 mm instead of 5000 mm -- 1800 mm off from both the
 * true point and its mirror across A-B, (-840,1120) at 1400 mm from MAIN. */
static void test_contradicting_third_reference_leaves_node_unsolved(void)
{
    nrf_geometry_node_input_t inputs[4] = {
        {.node_id = 0U, .z_mm = 0},
        {.node_id = 1U, .z_mm = 0},
        {.node_id = 2U, .z_mm = 0},
        {.node_id = 3U, .z_mm = 0},
    };
    nrf_cs_pair_result_t pairs[6] = {
        make_pair(0U, 1U, NRF_CS_PAIR_VALID, 3000),
        make_pair(0U, 2U, NRF_CS_PAIR_VALID, 4000),
        make_pair(1U, 2U, NRF_CS_PAIR_VALID, 5000),
        make_pair(0U, 3U, NRF_CS_PAIR_VALID, 3200),
        make_pair(1U, 3U, NRF_CS_PAIR_VALID, 4000),
        make_pair(2U, 3U, NRF_CS_PAIR_VALID, 3000),
    };
    nrf_geometry_solution_t solution;

    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, 6U, &solution) ==
           NRF_GEOMETRY_OK);
    assert(find_node(&solution, 1U)->solved);
    assert(find_node(&solution, 2U)->solved);
    assert(!find_node(&solution, 3U)->solved);
}


/* ---- MAX-144: weighted robust full-mesh refinement ---------------------- */

typedef struct { double x, y; } xy_t;

/* MAIN=(0,0) plus four REMOTEs on a 5x4 m rectangle-ish layout. */
static const xy_t s_truth[5] = {
    {0.0, 0.0}, {5000.0, 0.0}, {4800.0, 4100.0}, {-300.0, 3900.0}, {2400.0, 1800.0},
};

static int32_t true_distance(size_t a, size_t b)
{
    return (int32_t)lround(hypot(s_truth[a].x - s_truth[b].x, s_truth[a].y - s_truth[b].y));
}

static size_t full_mesh(nrf_cs_pair_result_t pairs[10])
{
    size_t count = 0U;
    for (size_t a = 0U; a < 5U; ++a) {
        for (size_t b = a + 1U; b < 5U; ++b) {
            pairs[count++] = make_pair((nrf_cs_node_id_t)a, (nrf_cs_node_id_t)b,
                                       NRF_CS_PAIR_VALID, true_distance(a, b));
        }
    }
    return count;
}

static void solve_mesh(const nrf_cs_pair_result_t *pairs, size_t count, nrf_geometry_solution_t *solution)
{
    nrf_geometry_node_input_t inputs[5];
    for (size_t i = 0U; i < 5U; ++i) {
        inputs[i].node_id = (nrf_cs_node_id_t)i;
        inputs[i].z_mm = 0;
    }
    assert(nrf_geometry_solver_solve(inputs, 5U, pairs, count, solution) == NRF_GEOMETRY_OK);
}

/* Distance-only layouts are defined up to rotation/reflection: compare
 * through every pairwise distance instead of raw coordinates. */
static double max_shape_error_mm(const nrf_geometry_solution_t *solution)
{
    double worst = 0.0;
    for (size_t a = 0U; a < 5U; ++a) {
        for (size_t b = a + 1U; b < 5U; ++b) {
            const nrf_geometry_node_position_t *na = find_node(solution, (nrf_cs_node_id_t)a);
            const nrf_geometry_node_position_t *nb = find_node(solution, (nrf_cs_node_id_t)b);
            assert(na->solved && nb->solved);
            const double solved = hypot((double)(na->x_mm - nb->x_mm), (double)(na->y_mm - nb->y_mm));
            const double error = fabs(solved - (double)true_distance(a, b));
            if (error > worst) worst = error;
        }
    }
    return worst;
}

static void test_refinement_exact_mesh_reports_uncertainty(void)
{
    nrf_cs_pair_result_t pairs[10];
    nrf_geometry_solution_t solution;
    solve_mesh(pairs, full_mesh(pairs), &solution);

    assert(solution.refined);
    assert(max_shape_error_mm(&solution) <= 2.0);
    assert(solution.max_residual_mm <= 2);
    assert(solution.inconsistent_pair_count == 0U);
    assert(find_node(&solution, 0U)->uncertainty_mm == 0);   /* fixed origin */
    for (nrf_cs_node_id_t id = 1U; id < 5U; ++id) {
        const int32_t sigma = find_node(&solution, id)->uncertainty_mm;
        /* A redundant mesh pins every node to around the range sigma. */
        assert(sigma > 0 && sigma < 600);
    }
}

/* One pair biased by +400 mm (within tolerance, so no node is rejected):
 * the refinement spreads it over the redundant mesh instead of letting the
 * sequential placement absorb it into one node's coordinates. */
static void test_refinement_absorbs_a_biased_pair(void)
{
    nrf_cs_pair_result_t pairs[10];
    const size_t count = full_mesh(pairs);
    for (size_t i = 0U; i < count; ++i) {
        if (pairs[i].node_a == 2U && pairs[i].node_b == 3U) pairs[i].distance_mm += 400;
    }
    nrf_geometry_solution_t solution;
    solve_mesh(pairs, count, &solution);
    assert(solution.refined);
    assert(solution.inconsistent_pair_count == 0U);
    /* The biased pair itself is partly absorbed by the flexible mesh, but
     * every OTHER distance stays close to the truth. */
    for (size_t a = 0U; a < 5U; ++a) {
        for (size_t b = a + 1U; b < 5U; ++b) {
            if (a == 2U && b == 3U) continue;
            const nrf_geometry_node_position_t *na = find_node(&solution, (nrf_cs_node_id_t)a);
            const nrf_geometry_node_position_t *nb = find_node(&solution, (nrf_cs_node_id_t)b);
            const double solved = hypot((double)(na->x_mm - nb->x_mm), (double)(na->y_mm - nb->y_mm));
            assert(fabs(solved - (double)true_distance(a, b)) < 250.0);
        }
    }
}

/* A single gross outlier (+3 m) on one pair: the node is still placed from
 * its other references, the robust refinement keeps the layout accurate,
 * and the bad pair is reported (the START gate then refuses it). */
static void test_refinement_survives_single_gross_outlier(void)
{
    nrf_cs_pair_result_t pairs[10];
    const size_t count = full_mesh(pairs);
    for (size_t i = 0U; i < count; ++i) {
        if (pairs[i].node_a == 3U && pairs[i].node_b == 4U) pairs[i].distance_mm += 3000;
    }
    nrf_geometry_solution_t solution;
    solve_mesh(pairs, count, &solution);
    for (nrf_cs_node_id_t id = 0U; id < 5U; ++id) {
        assert(find_node(&solution, id)->solved);
    }
    /* Every TRUE distance except the outlier pair's is still recovered. */
    for (size_t a = 0U; a < 5U; ++a) {
        for (size_t b = a + 1U; b < 5U; ++b) {
            if (a == 3U && b == 4U) continue;
            const nrf_geometry_node_position_t *na = find_node(&solution, (nrf_cs_node_id_t)a);
            const nrf_geometry_node_position_t *nb = find_node(&solution, (nrf_cs_node_id_t)b);
            const double solved = hypot((double)(na->x_mm - nb->x_mm), (double)(na->y_mm - nb->y_mm));
            assert(fabs(solved - (double)true_distance(a, b)) < 300.0);
        }
    }
    assert(solution.inconsistent_pair_count >= 1U);
}

/* Two MISSING pairs (distance field 0) are never used as 0 mm, and the
 * placement order adapts: node 2 (no pair to the axis node 1) is placed on
 * a later pass once nodes 3 and 4 are known. With 1-3 and 2-4 missing
 * instead, nodes 3 and 4 each have only two references (a mirror ambiguity
 * one at a time); their 3-4 pair would resolve them jointly only if exactly
 * one mirror combination fit it -- here node 3's mirror also fits within
 * 371 mm (< tolerance), so both stay unsolved instead of being guessed. */
static void test_refinement_with_missing_pairs(void)
{
    nrf_cs_pair_result_t pairs[10];
    const size_t count = full_mesh(pairs);
    for (size_t i = 0U; i < count; ++i) {
        if ((pairs[i].node_a == 1U && pairs[i].node_b == 2U) ||
            (pairs[i].node_a == 3U && pairs[i].node_b == 4U)) {
            pairs[i].state = NRF_CS_PAIR_MISSING;
            pairs[i].distance_mm = 0;
        }
    }
    nrf_geometry_solution_t solution;
    solve_mesh(pairs, count, &solution);
    assert(solution.refined);
    assert(max_shape_error_mm(&solution) <= 3.0);

    nrf_cs_pair_result_t joint[10];
    const size_t joint_count = full_mesh(joint);
    for (size_t i = 0U; i < joint_count; ++i) {
        if ((joint[i].node_a == 1U && joint[i].node_b == 3U) ||
            (joint[i].node_a == 2U && joint[i].node_b == 4U)) {
            joint[i].state = NRF_CS_PAIR_MISSING;
            joint[i].distance_mm = 0;
        }
    }
    solve_mesh(joint, joint_count, &solution);
    assert(!find_node(&solution, 3U)->solved);
    assert(!find_node(&solution, 4U)->solved);
}

/* Near-degenerate: every REMOTE almost on one line through MAIN. Along-line
 * coordinates are well determined; the perpendicular one is not, and that
 * must show up as a large uncertainty rather than false confidence. */
static void test_near_collinear_layout_reports_large_uncertainty(void)
{
    const xy_t line[4] = { {0.0, 0.0}, {2000.0, 0.0}, {4000.0, 5.0}, {6000.0, -5.0} };
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    size_t count = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        inputs[i].node_id = (nrf_cs_node_id_t)i;
        inputs[i].z_mm = 0;
        for (size_t j = i + 1U; j < 4U; ++j) {
            pairs[count++] = make_pair((nrf_cs_node_id_t)i, (nrf_cs_node_id_t)j, NRF_CS_PAIR_VALID,
                (int32_t)lround(hypot(line[i].x - line[j].x, line[i].y - line[j].y)));
        }
    }
    nrf_geometry_solution_t solution;
    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, count, &solution) == NRF_GEOMETRY_OK);
    /* At least one off-axis node's position is poorly determined. */
    int32_t largest = 0;
    for (nrf_cs_node_id_t id = 1U; id < 4U; ++id) {
        if (find_node(&solution, id)->uncertainty_mm > largest) largest = find_node(&solution, id)->uncertainty_mm;
    }
    assert(largest > 1000);
}


/* Joint mirror resolution does fire when the shared pair is decisive:
 * MAIN=(0,0), A=(4000,0), B=(0,4000); C and D pair only with MAIN/A (resp.
 * MAIN/B) plus each other. Only one mirror combination fits C-D. */
static void test_joint_mirror_resolution_through_shared_pair(void)
{
    const xy_t points[5] = { {0, 0}, {4000, 0}, {0, 4000}, {3000, 3500}, {3500, 5000} };
    nrf_geometry_node_input_t inputs[5];
    nrf_cs_pair_result_t pairs[8];
    size_t count = 0U;
    const size_t edges[8][2] = { {0,1}, {0,2}, {1,2}, {0,3}, {1,3}, {0,4}, {2,4}, {3,4} };
    for (size_t i = 0U; i < 5U; ++i) {
        inputs[i].node_id = (nrf_cs_node_id_t)i;
        inputs[i].z_mm = 0;
    }
    for (size_t e = 0U; e < 8U; ++e) {
        const xy_t a = points[edges[e][0]];
        const xy_t b = points[edges[e][1]];
        pairs[count++] = make_pair((nrf_cs_node_id_t)edges[e][0], (nrf_cs_node_id_t)edges[e][1],
            NRF_CS_PAIR_VALID, (int32_t)lround(hypot(a.x - b.x, a.y - b.y)));
    }
    nrf_geometry_solution_t solution;
    assert(nrf_geometry_solver_solve(inputs, 5U, pairs, count, &solution) == NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *c = find_node(&solution, 3U);
    const nrf_geometry_node_position_t *d = find_node(&solution, 4U);
    assert(c->solved && d->solved);
    assert(iabs(c->x_mm - 3000) <= 5 && iabs(c->y_mm - 3500) <= 5);
    assert(iabs(d->x_mm - 3500) <= 5 && iabs(d->y_mm - 5000) <= 5);
}

static void test_zero_horizontal_first_remote_does_not_claim_rotation_axis(void)
{
    nrf_geometry_node_input_t vertical_inputs[2] = {{.node_id = 0U}, {.node_id = 1U, .z_mm = 1000}};
    nrf_cs_pair_result_t vertical_pair = make_pair(0U, 1U, NRF_CS_PAIR_VALID, 1000);
    nrf_geometry_solution_t vertical_solution;
    assert(nrf_geometry_solver_solve_partial(vertical_inputs, 2U, &vertical_pair, 1U, &vertical_solution) == NRF_GEOMETRY_OK);
    assert(find_node(&vertical_solution, 1U)->solved);
    assert(find_node(&vertical_solution, 1U)->x_mm == 0 && find_node(&vertical_solution, 1U)->y_mm == 0);
    const int32_t xy[5][2] = {{0, 0}, {0, 0}, {3000, 0}, {0, 4000}, {3000, 4000}};
    for (int32_t height = -1000; height <= 1000; height += 1000) {
        for (size_t n = 4U; n <= 5U; ++n) {
            nrf_geometry_node_input_t inputs[5];
            nrf_cs_pair_result_t pairs[NRF_CS_MAX_PAIRS];
            size_t count = 0U;
            for (size_t i = 0U; i < n; ++i) {
                inputs[i] = (nrf_geometry_node_input_t){.node_id = (uint8_t)i, .z_mm = i == 1U ? height : 0};
                for (size_t j = 0U; j < i; ++j) {
                    const double dz = (double)inputs[i].z_mm - inputs[j].z_mm;
                    const double d = hypot(hypot((double)xy[i][0] - xy[j][0], (double)xy[i][1] - xy[j][1]), dz);
                    pairs[count++] = make_pair((uint8_t)j, (uint8_t)i, NRF_CS_PAIR_VALID, (int32_t)lround(d));
                }
            }
            nrf_geometry_solution_t solution;
            assert(nrf_geometry_solver_solve(inputs, n, pairs, count, &solution) == NRF_GEOMETRY_OK);
            for (size_t i = 0U; i < n; ++i) {
                const nrf_geometry_node_position_t *p = find_node(&solution, (uint8_t)i);
                assert(p != NULL && p->solved);
                assert(hypot((double)p->x_mm - xy[i][0], (double)p->y_mm - xy[i][1]) <= 5.0);
                assert(nrf_geometry_position_uncertainty_acceptable(p->uncertainty_mm));
            }
            assert(solution.refined && solution.inconsistent_pair_count == 0U && solution.max_residual_mm <= 1);
            const nrf_geometry_node_input_t main_input = inputs[0];
            for (size_t i = 0U; i + 1U < n; ++i) inputs[i] = inputs[i + 1U];
            inputs[n - 1U] = main_input;
            assert(nrf_geometry_solver_solve(inputs, n, pairs, count, &solution) == NRF_GEOMETRY_OK);
            const double reflection = find_node(&solution, 3U)->y_mm < 0 ? -1.0 : 1.0;
            for (size_t i = 0U; i < n; ++i) {
                const nrf_geometry_node_position_t *p = &solution.nodes[i];
                assert(p->node_id == inputs[i].node_id && p->solved);
                assert(hypot((double)p->x_mm - xy[p->node_id][0], reflection * p->y_mm - xy[p->node_id][1]) <= 5.0);
                assert(nrf_geometry_position_uncertainty_acceptable(p->uncertainty_mm));
            }
            assert(solution.inconsistent_pair_count == 0U && solution.max_residual_mm <= 1);
        }
    }
}

int main(void)
{
    assert(!nrf_geometry_position_uncertainty_acceptable(-1));
    assert(nrf_geometry_position_uncertainty_acceptable(0));
    assert(nrf_geometry_position_uncertainty_acceptable(750));
    assert(!nrf_geometry_position_uncertainty_acceptable(751));
    assert(!nrf_geometry_position_uncertainty_acceptable(INT32_MAX));
    test_rejects_missing_main_and_bad_arguments();
    test_main_alone_is_the_origin();
    test_less_than_three_remote_nodes_return_unsolved();
    test_single_reference_places_along_arbitrary_heading();
    test_isolated_node_with_no_valid_pair_stays_unsolved();
    test_missing_pair_state_is_not_treated_as_zero_distance();
    test_rejects_negative_valid_distance_and_duplicate_node_ids();
    test_overflowing_position_is_not_marked_solved();
    test_trilateration_with_mirror_disambiguation();
    test_z_passes_through_unmodified();
    test_deterministic_randomized_noise_and_node_order();
    test_impossible_height_difference_is_not_solved();
    test_disjoint_reference_circles_leave_node_unsolved();
    test_near_miss_within_tolerance_is_solved_with_residual();
    test_second_single_reference_node_stays_unsolved();
    test_two_reference_mirror_ambiguity_after_reflection_fixed();
    test_contradicting_third_reference_leaves_node_unsolved();
    test_refinement_exact_mesh_reports_uncertainty();
    test_refinement_absorbs_a_biased_pair();
    test_refinement_survives_single_gross_outlier();
    test_refinement_with_missing_pairs();
    test_near_collinear_layout_reports_large_uncertainty();
    test_joint_mirror_resolution_through_shared_pair();
    test_zero_horizontal_first_remote_does_not_claim_rotation_axis();
    return 0;
}
