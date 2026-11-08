#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nrf_geometry_solver.h"

/*
 * REMOTE_ONLY geometry: the explicit anchor API. The legacy solve()/solve_partial() fixtures stay in
 * nrf_geometry_solver_test.c, untouched; these tests pin what the anchor API adds and that anchor 0 is the old solver.
 */

typedef struct {
    int32_t x, y, z;
} pt_t;

static nrf_cs_pair_result_t make_pair(nrf_cs_node_id_t a, nrf_cs_node_id_t b, nrf_cs_pair_state_t state, int32_t distance_mm)
{
    nrf_cs_pair_result_t pair = {0};
    pair.node_a = a;
    pair.node_b = b;
    pair.state = state;
    pair.distance_mm = distance_mm;
    return pair;
}

static int32_t distance_mm(const pt_t *a, const pt_t *b)
{
    const double dx = (double)a->x - b->x, dy = (double)a->y - b->y, dz = (double)a->z - b->z;
    return (int32_t)lround(sqrt(dx * dx + dy * dy + dz * dz));
}

/* Full mesh over `n` nodes with the given ids and true positions. */
static size_t build(const nrf_cs_node_id_t *ids, const pt_t *points, size_t n,
                    nrf_geometry_node_input_t *inputs, nrf_cs_pair_result_t *pairs)
{
    size_t count = 0U;
    for (size_t i = 0U; i < n; ++i) inputs[i] = (nrf_geometry_node_input_t){.node_id = ids[i], .z_mm = points[i].z};
    for (size_t i = 0U; i < n; ++i)
        for (size_t j = i + 1U; j < n; ++j)
            pairs[count++] = make_pair(ids[i], ids[j], NRF_CS_PAIR_VALID, distance_mm(&points[i], &points[j]));
    return count;
}

static const nrf_geometry_node_position_t *find_node(const nrf_geometry_solution_t *solution, nrf_cs_node_id_t id)
{
    for (size_t i = 0U; i < solution->node_count; ++i)
        if (solution->nodes[i].node_id == id) return &solution->nodes[i];
    return NULL;
}

static int32_t solved_distance(const nrf_geometry_solution_t *solution, nrf_cs_node_id_t a, nrf_cs_node_id_t b)
{
    const nrf_geometry_node_position_t *na = find_node(solution, a), *nb = find_node(solution, b);
    assert(na != NULL && nb != NULL && na->solved && nb->solved);
    const pt_t pa = {na->x_mm, na->y_mm, na->z_mm}, pb = {nb->x_mm, nb->y_mm, nb->z_mm};
    return distance_mm(&pa, &pb);
}

/* An irregular quadrilateral: no symmetry the solver could hide behind. */
static const pt_t k_layout[4] = {{0, 0, 0}, {1500, 0, 0}, {1400, 1200, 0}, {-300, 1000, 0}};

static void expect_layout_reproduced(const nrf_geometry_solution_t *solution, const nrf_cs_node_id_t *ids, const pt_t *truth)
{
    for (size_t i = 0U; i < 4U; ++i)
        for (size_t j = i + 1U; j < 4U; ++j)
            assert(abs(solved_distance(solution, ids[i], ids[j]) - distance_mm(&truth[i], &truth[j])) <= 3);
}

static void anchor_g1_geometry_four_nodes(void)
{
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t solution;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);
    assert(count == 6U);

    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(solution.node_count == 4U);
    assert(solution.anchor_node_id == 1U);
    const nrf_geometry_node_position_t *g1 = find_node(&solution, 1U);
    assert(g1 != NULL && g1->solved && g1->x_mm == 0 && g1->y_mm == 0 && g1->uncertainty_mm == 0);
    for (size_t i = 0U; i < 4U; ++i) {
        const nrf_geometry_node_position_t *node = find_node(&solution, ids[i]);
        assert(node != NULL && node->node_id == ids[i] && node->solved); /* real ids, nobody renamed */
    }
    expect_layout_reproduced(&solution, ids, k_layout);
    assert(solution.inconsistent_pair_count == 0U);
    assert(nrf_geometry_metrics_consistent(solution.inconsistent_pair_count, solution.max_residual_mm));
    assert(find_node(&solution, 0U) == NULL); /* no node 0 appears */

    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(solution.anchor_node_id == 1U && find_node(&solution, 1U)->solved);
}

static bool same_node(const nrf_geometry_node_position_t *a, const nrf_geometry_node_position_t *b)
{
    return a->solved == b->solved && a->x_mm == b->x_mm && a->y_mm == b->y_mm && a->z_mm == b->z_mm &&
           a->residual_mm == b->residual_mm && a->uncertainty_mm == b->uncertainty_mm;
}

static void anchor_0_legacy_same_positions(void)
{
    const nrf_cs_node_id_t ids[4] = {0U, 1U, 2U, 3U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t legacy, anchored, partial_legacy, partial_anchored;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);

    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, count, &legacy) == NRF_GEOMETRY_OK);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 0U, &anchored) == NRF_GEOMETRY_OK);
    assert(legacy.node_count == anchored.node_count && legacy.max_residual_mm == anchored.max_residual_mm);
    assert(legacy.inconsistent_pair_count == anchored.inconsistent_pair_count);
    assert(legacy.refined == anchored.refined && legacy.refine_iterations == anchored.refine_iterations);
    for (size_t i = 0U; i < legacy.node_count; ++i) {
        assert(legacy.nodes[i].node_id == anchored.nodes[i].node_id);
        assert(same_node(&legacy.nodes[i], &anchored.nodes[i]));
    }
    assert(legacy.anchor_node_id == 0U && anchored.anchor_node_id == 0U);

    assert(nrf_geometry_solver_solve_partial(inputs, 4U, pairs, count, &partial_legacy) == NRF_GEOMETRY_OK);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 4U, pairs, count, 0U, &partial_anchored) == NRF_GEOMETRY_OK);
    for (size_t i = 0U; i < partial_legacy.node_count; ++i) assert(same_node(&partial_legacy.nodes[i], &partial_anchored.nodes[i]));

    /* The ids are labels only: the same mesh under ids 1..4 with anchor 1 gives the very same layout. */
    const nrf_cs_node_id_t shifted[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t shifted_inputs[4];
    nrf_cs_pair_result_t shifted_pairs[6];
    nrf_geometry_solution_t shifted_solution;
    const size_t shifted_count = build(shifted, k_layout, 4U, shifted_inputs, shifted_pairs);
    assert(nrf_geometry_solver_solve_with_anchor(
        shifted_inputs, 4U, shifted_pairs, shifted_count, 1U, &shifted_solution) == NRF_GEOMETRY_OK);
    for (size_t i = 0U; i < 4U; ++i) assert(same_node(&legacy.nodes[i], &shifted_solution.nodes[i]));
}

static void missing_anchor_fails(void)
{
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t solution;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);

    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 9U, &solution) == NRF_GEOMETRY_ERR_NO_ANCHOR);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 4U, pairs, count, 9U, &solution) == NRF_GEOMETRY_ERR_NO_ANCHOR);
    /* Node 0 is not special in the anchor API: with ids 1..4 it is simply not there. */
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 0U, &solution) == NRF_GEOMETRY_ERR_NO_ANCHOR);
    /* The legacy API keeps its own error for the same input. */
    assert(nrf_geometry_solver_solve(inputs, 4U, pairs, count, &solution) == NRF_GEOMETRY_ERR_NO_MAIN);
    assert(nrf_geometry_solver_solve_partial(inputs, 4U, pairs, count, &solution) == NRF_GEOMETRY_ERR_NO_MAIN);
}

static void bad_arguments_fail(void)
{
    nrf_geometry_node_input_t inputs[1] = {{.node_id = 1U}};
    nrf_geometry_solution_t solution;
    assert(nrf_geometry_solver_solve_with_anchor(NULL, 1U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 0U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 1U, NULL, 0U, 1U, NULL) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 1U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(find_node(&solution, 1U)->solved && solution.anchor_node_id == 1U);
}

static void duplicate_ids_fail(void)
{
    nrf_geometry_node_input_t inputs[3] = {{.node_id = 1U}, {.node_id = 2U}, {.node_id = 2U}};
    nrf_geometry_solution_t solution;
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 3U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 3U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    /* A duplicated anchor is the same error, not a silently picked first copy. */
    nrf_geometry_node_input_t twice[2] = {{.node_id = 1U}, {.node_id = 1U}};
    assert(nrf_geometry_solver_solve_partial_with_anchor(twice, 2U, NULL, 0U, 1U, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
}

static void missing_pair_does_not_become_zero(void)
{
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    const nrf_cs_node_id_t legacy_ids[4] = {0U, 1U, 2U, 3U};
    nrf_geometry_node_input_t inputs[4], legacy_inputs[4];
    nrf_cs_pair_result_t pairs[6], legacy_pairs[6];
    nrf_geometry_solution_t solution, legacy;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);
    build(legacy_ids, k_layout, 4U, legacy_inputs, legacy_pairs);
    for (size_t i = 0U; i < count; ++i) {
        if (pairs[i].node_a == 2U && pairs[i].node_b == 3U) {
            pairs[i].state = NRF_CS_PAIR_MISSING;
            pairs[i].distance_mm = 0; /* what an unmeasured pair carries: not a 0 mm reading */
            legacy_pairs[i].state = NRF_CS_PAIR_MISSING;
            legacy_pairs[i].distance_mm = 0;
        }
    }
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(nrf_geometry_solver_solve_partial(legacy_inputs, 4U, legacy_pairs, count, &legacy) == NRF_GEOMETRY_OK);
    for (size_t i = 0U; i < 4U; ++i) assert(same_node(&solution.nodes[i], &legacy.nodes[i])); /* same rules as before */
    assert(solution.inconsistent_pair_count == 0U);
    /* A missing edge is never read as "the two nodes are together": a node it cannot place stays unsolved. */
    if (find_node(&solution, 2U)->solved && find_node(&solution, 3U)->solved)
        assert(solved_distance(&solution, 2U, 3U) > 1000);
    for (size_t i = 0U; i < 4U; ++i)
        if (!solution.nodes[i].solved) assert(solution.nodes[i].x_mm == 0 && solution.nodes[i].y_mm == 0);
}

/* True when the result may be used as a measured layout: every node placed and the metrics consistent. */
static bool geometry_usable(const nrf_geometry_solution_t *solution)
{
    for (size_t i = 0U; i < solution->node_count; ++i)
        if (!solution->nodes[i].solved) return false;
    return nrf_geometry_metrics_consistent(solution->inconsistent_pair_count, solution->max_residual_mm);
}

static void inconsistent_pair_flagged(void)
{
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t solution;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(geometry_usable(&solution)); /* the clean mesh is usable, so the next assertion means something */

    for (size_t i = 0U; i < count; ++i)
        if (pairs[i].node_a == 3U && pairs[i].node_b == 4U) pairs[i].distance_mm += 4000;
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    /* One contradicting measurement: the node it cannot be reconciled with is left unsolved (never forced). */
    assert(!geometry_usable(&solution));
    assert(!find_node(&solution, 4U)->solved);

    /* A smaller error is kept and shows as a residual instead of being hidden. */
    for (size_t i = 0U; i < count; ++i)
        if (pairs[i].node_a == 3U && pairs[i].node_b == 4U) pairs[i].distance_mm -= 3400; /* +600 mm in total */
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(solution.max_residual_mm >= 100);
}

/* Three collinear nodes and two off-axis ones: the mirror of each off-axis node is undetermined (legacy rule, kept). */
static void degenerate_layout_reports_ambiguity(void)
{
    static const pt_t points[5] = {{0, 0, 0}, {4000, 0, 0}, {8000, 0, 0}, {2000, 3000, 0}, {6000, -3000, 0}};
    const nrf_cs_node_id_t ids[5] = {1U, 2U, 3U, 4U, 5U};
    nrf_geometry_node_input_t inputs[5];
    nrf_cs_pair_result_t pairs[10];
    nrf_geometry_solution_t solution;
    const size_t count = build(ids, points, 5U, inputs, pairs);
    pairs[9].state = NRF_CS_PAIR_MISSING; /* the (4,5) edge that would tie the two mirrors together */
    pairs[9].distance_mm = 0;
    assert(count == 10U && pairs[9].node_a == 4U && pairs[9].node_b == 5U);

    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 5U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(!find_node(&solution, 4U)->solved || !find_node(&solution, 5U)->solved); /* ambiguity is not guessed away */
    for (size_t i = 0U; i < solution.node_count; ++i)
        if (!solution.nodes[i].solved) assert(solution.nodes[i].x_mm == 0 && solution.nodes[i].y_mm == 0);

    pairs[9].state = NRF_CS_PAIR_VALID;
    pairs[9].distance_mm = distance_mm(&points[3], &points[4]);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 5U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    assert(find_node(&solution, 4U)->solved && find_node(&solution, 5U)->solved);
    assert((int64_t)find_node(&solution, 4U)->y_mm * find_node(&solution, 5U)->y_mm < 0);
}

static void anchor_not_always_lowest_api_explicit(void)
{
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t by_g1, by_g3;
    const size_t count = build(ids, k_layout, 4U, inputs, pairs);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &by_g1) == NRF_GEOMETRY_OK);
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 3U, &by_g3) == NRF_GEOMETRY_OK);

    assert(by_g3.anchor_node_id == 3U);
    const nrf_geometry_node_position_t *g3 = find_node(&by_g3, 3U);
    assert(g3->solved && g3->x_mm == 0 && g3->y_mm == 0 && g3->uncertainty_mm == 0);
    expect_layout_reproduced(&by_g3, ids, k_layout);
    /* Same shape, different origin: G1 is no longer at (0,0). */
    assert(find_node(&by_g3, 1U)->x_mm != 0 || find_node(&by_g3, 1U)->y_mm != 0);
    assert(abs(solved_distance(&by_g1, 1U, 4U) - solved_distance(&by_g3, 1U, 4U)) <= 3);
}

static void anchor_keeps_its_own_height_and_recording_minimum_applies(void)
{
    static const pt_t points[4] = {{0, 0, 250}, {1500, 0, 100}, {1400, 1200, -150}, {-300, 1000, 40}};
    const nrf_cs_node_id_t ids[4] = {1U, 2U, 3U, 4U};
    nrf_geometry_node_input_t inputs[4];
    nrf_cs_pair_result_t pairs[6];
    nrf_geometry_solution_t solution;
    const size_t count = build(ids, points, 4U, inputs, pairs);

    assert(nrf_geometry_solver_solve_with_anchor(inputs, 4U, pairs, count, 1U, &solution) == NRF_GEOMETRY_OK);
    for (size_t i = 0U; i < 4U; ++i) assert(find_node(&solution, ids[i])->z_mm == points[i].z); /* anchor z not forced to 0 */
    for (size_t i = 0U; i < 4U; ++i)
        for (size_t j = i + 1U; j < 4U; ++j)
            assert(abs(solved_distance(&solution, ids[i], ids[j]) - distance_mm(&points[i], &points[j])) <= 3);

    /* The recording-grade solve still needs the anchor plus three more nodes; the partial one does not. */
    assert(nrf_geometry_solver_solve_with_anchor(inputs, 3U, pairs, 3U, 1U, &solution) == NRF_GEOMETRY_UNS_INSUFFICIENT_NODES);
    assert(nrf_geometry_solver_solve_partial_with_anchor(inputs, 3U, pairs, 3U, 1U, &solution) == NRF_GEOMETRY_OK);
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case_t;

#define CASE(fn) {#fn, fn}

/* No argument runs everything; one argument runs the named case (to show a single RED/GREEN case). */
int main(int argc, char **argv)
{
    static const test_case_t cases[] = {
        CASE(anchor_g1_geometry_four_nodes),
        CASE(anchor_0_legacy_same_positions),
        CASE(missing_anchor_fails),
        CASE(bad_arguments_fail),
        CASE(duplicate_ids_fail),
        CASE(missing_pair_does_not_become_zero),
        CASE(inconsistent_pair_flagged),
        CASE(degenerate_layout_reports_ambiguity),
        CASE(anchor_not_always_lowest_api_explicit),
        CASE(anchor_keeps_its_own_height_and_recording_minimum_applies),
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
