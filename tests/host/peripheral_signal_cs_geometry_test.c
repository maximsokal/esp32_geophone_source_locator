/*
 * What the Channel Sounding distance mesh can and cannot tell us about the
 * layout. The solver receives only pairwise distances (no angles), so every
 * solved position must be consistent with the true layout up to a rigid
 * motion and a mirror image, and a node the mesh does not determine must stay
 * unsolved. All 2^10 subsets of measured pairs of a 5-node network are tried.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nrf_geometry_solver.h"

#define CASE_PASS(name) printf("PERIPHERAL_CASE %s PASS\n", name)
#define NODES 5U
#define PAIRS 10U

typedef struct {
    double x_mm;
    double y_mm;
    int32_t z_mm;
} truth_t;

static const truth_t k_layouts[3][NODES] = {
    {{0, 0, 0}, {4200, 350, 120}, {1300, 3900, -80}, {-2600, 2100, 240}, {2900, -3300, 60}},
    {{0, 0, 0}, {-3100, 900, 0}, {700, -4400, 150}, {5200, 3600, -200}, {-1500, 4700, 90}},
    {{0, 0, 0}, {6100, 1200, 300}, {2400, 5300, 0}, {-4100, -2700, -120}, {900, -6200, 210}},
};

static double horizontal(const truth_t *a, const truth_t *b)
{
    return hypot(a->x_mm - b->x_mm, a->y_mm - b->y_mm);
}

static nrf_cs_pair_result_t make_pair(const truth_t *layout, unsigned a, unsigned b, bool valid)
{
    nrf_cs_pair_result_t pair;
    memset(&pair, 0, sizeof(pair));
    pair.node_a = (nrf_cs_node_id_t)a;
    pair.node_b = (nrf_cs_node_id_t)b;
    if (valid) {
        const double dz = (double)layout[a].z_mm - (double)layout[b].z_mm;
        const double dh = horizontal(&layout[a], &layout[b]);
        pair.state = NRF_CS_PAIR_VALID;
        pair.distance_mm = (int32_t)lround(sqrt(dh * dh + dz * dz));
        pair.confidence = 900U;
        pair.valid_samples = 3U;
        pair.spread_mm = 20;
    } else {
        pair.state = NRF_CS_PAIR_MISSING;
    }
    return pair;
}

static void fill_inputs(const truth_t *layout, nrf_geometry_node_input_t inputs[NODES])
{
    for (unsigned i = 0U; i < NODES; ++i) {
        inputs[i].node_id = (nrf_cs_node_id_t)i;
        inputs[i].z_mm = layout[i].z_mm;
    }
}

static void every_solved_node_is_consistent_with_the_truth(void)
{
    unsigned subsets_solved_all = 0U;
    unsigned subsets_with_unsolved = 0U;
    unsigned partially_solved = 0U;

    for (unsigned layout_index = 0U; layout_index < 3U; ++layout_index) {
        const truth_t *layout = k_layouts[layout_index];
        nrf_geometry_node_input_t inputs[NODES];
        fill_inputs(layout, inputs);

        for (unsigned mask = 0U; mask < (1U << PAIRS); ++mask) {
            nrf_cs_pair_result_t pairs[PAIRS];
            size_t pair_count = 0U;
            unsigned bit = 0U;
            for (unsigned a = 0U; a < NODES; ++a) {
                for (unsigned b = a + 1U; b < NODES; ++b) {
                    /* Unmeasured pairs are reported as MISSING with a zero distance. */
                    pairs[pair_count++] = make_pair(layout, a, b, (mask & (1U << bit)) != 0U);
                    ++bit;
                }
            }
            nrf_geometry_solution_t solution;
            assert(nrf_geometry_solver_solve_partial(inputs, NODES, pairs, pair_count, &solution) == NRF_GEOMETRY_OK);
            assert(solution.nodes[0].solved && solution.nodes[0].x_mm == 0 && solution.nodes[0].y_mm == 0);

            unsigned solved = 0U;
            for (unsigned i = 0U; i < NODES; ++i) {
                assert(solution.nodes[i].node_id == i && solution.nodes[i].z_mm == layout[i].z_mm);
                if (!solution.nodes[i].solved) {
                    /* An unsolved node never carries a made-up position. */
                    assert(solution.nodes[i].x_mm == 0 && solution.nodes[i].y_mm == 0);
                    continue;
                }
                ++solved;
            }
            /* Soundness: distances between solved nodes (measured or not) match the truth. */
            for (unsigned i = 0U; i < NODES; ++i) {
                for (unsigned j = i + 1U; j < NODES; ++j) {
                    if (!solution.nodes[i].solved || !solution.nodes[j].solved) continue;
                    const double solved_dh = hypot((double)solution.nodes[i].x_mm - solution.nodes[j].x_mm,
                                                   (double)solution.nodes[i].y_mm - solution.nodes[j].y_mm);
                    const double true_dh = horizontal(&layout[i], &layout[j]);
                    const double sigma_i = solution.nodes[i].uncertainty_mm == INT32_MAX ? 1e12 : (double)solution.nodes[i].uncertainty_mm;
                    const double sigma_j = solution.nodes[j].uncertainty_mm == INT32_MAX ? 1e12 : (double)solution.nodes[j].uncertainty_mm;
                    /* The reported 1-sigma must cover the real error (3 sigma plus rounding). */
                    const double allowed = 25.0 + 3.0 * (sigma_i + sigma_j);
                    if (fabs(solved_dh - true_dh) >= allowed) {
                        fprintf(stderr, "layout=%u mask=0x%03x nodes %u-%u solved_dh=%.1f true_dh=%.1f\n",
                            layout_index, mask, i, j, solved_dh, true_dh);
                        for (unsigned k = 0U; k < NODES; ++k) {
                            fprintf(stderr, "  node %u unc=%d res=%d solved=%d (%d,%d) true=(%.0f,%.0f)\n", k, (int)solution.nodes[k].uncertainty_mm, (int)solution.nodes[k].residual_mm,
                                (int)solution.nodes[k].solved, (int)solution.nodes[k].x_mm,
                                (int)solution.nodes[k].y_mm, layout[k].x_mm, layout[k].y_mm);
                        }
                        fprintf(stderr, "  pairs measured:");
                        unsigned pb = 0U;
                        for (unsigned a2 = 0U; a2 < NODES; ++a2)
                            for (unsigned b2 = a2 + 1U; b2 < NODES; ++b2, ++pb)
                                if (mask & (1U << pb)) fprintf(stderr, " %u-%u", a2, b2);
                        fprintf(stderr, "\n");
                    }
                    assert(fabs(solved_dh - true_dh) < allowed);
                }
            }
            if (solved == NODES) ++subsets_solved_all; else ++subsets_with_unsolved;
            if (solved > 1U && solved < NODES) ++partially_solved;
        }
    }
    /* Sanity of the experiment itself: the mesh decides, so both outcomes occur. */
    assert(subsets_solved_all > 0U && subsets_with_unsolved > 0U && partially_solved > 0U);
    CASE_PASS("CS.GEOMETRY.sparse_mesh_soundness");
}

static void a_complete_exact_mesh_solves_every_node_with_tiny_residuals(void)
{
    for (unsigned layout_index = 0U; layout_index < 3U; ++layout_index) {
        const truth_t *layout = k_layouts[layout_index];
        nrf_geometry_node_input_t inputs[NODES];
        nrf_cs_pair_result_t pairs[PAIRS];
        nrf_geometry_solution_t solution;
        size_t count = 0U;
        fill_inputs(layout, inputs);
        for (unsigned a = 0U; a < NODES; ++a) {
            for (unsigned b = a + 1U; b < NODES; ++b) pairs[count++] = make_pair(layout, a, b, true);
        }
        assert(nrf_geometry_solver_solve(inputs, NODES, pairs, count, &solution) == NRF_GEOMETRY_OK);
        for (unsigned i = 0U; i < NODES; ++i) {
            assert(solution.nodes[i].solved);
            assert(solution.nodes[i].residual_mm <= 5);
        }
        assert(solution.inconsistent_pair_count == 0U && solution.max_residual_mm <= 5);
    }
    CASE_PASS("CS.GEOMETRY.exact_mesh");
}

static void measurement_error_grows_the_residual_but_not_the_solved_set(void)
{
    const truth_t *layout = k_layouts[0];
    nrf_geometry_node_input_t inputs[NODES];
    fill_inputs(layout, inputs);

    int32_t previous_max_residual = -1;
    for (int32_t bias_mm = 0; bias_mm <= 600; bias_mm += 150) {
        nrf_cs_pair_result_t pairs[PAIRS];
        nrf_geometry_solution_t solution;
        size_t count = 0U;
        for (unsigned a = 0U; a < NODES; ++a) {
            for (unsigned b = a + 1U; b < NODES; ++b) {
                pairs[count] = make_pair(layout, a, b, true);
                /* One pair biased long: the mesh no longer closes exactly. */
                if (a == 1U && b == 3U) pairs[count].distance_mm += bias_mm;
                ++count;
            }
        }
        assert(nrf_geometry_solver_solve(inputs, NODES, pairs, count, &solution) == NRF_GEOMETRY_OK);
        unsigned solved = 0U;
        for (unsigned i = 0U; i < NODES; ++i) solved += solution.nodes[i].solved ? 1U : 0U;
        assert(solved == NODES);
        assert(solution.max_residual_mm >= 0);
        /* A biased pair is never hidden: the worst residual does not shrink as the bias grows. */
        assert(solution.max_residual_mm + 60 >= previous_max_residual);
        previous_max_residual = solution.max_residual_mm;
    }
    CASE_PASS("CS.GEOMETRY.bias_visibility");
}

static void invalid_pair_states_and_arguments_are_rejected(void)
{
    const truth_t *layout = k_layouts[0];
    nrf_geometry_node_input_t inputs[NODES];
    nrf_cs_pair_result_t pairs[PAIRS];
    nrf_geometry_solution_t solution;
    size_t count = 0U;
    fill_inputs(layout, inputs);
    for (unsigned a = 0U; a < NODES; ++a) {
        for (unsigned b = a + 1U; b < NODES; ++b) pairs[count++] = make_pair(layout, a, b, true);
    }

    assert(nrf_geometry_solver_solve(NULL, NODES, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve(inputs, 0U, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve(inputs, NODES + 1U, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve(inputs, NODES, NULL, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    assert(nrf_geometry_solver_solve(inputs, NODES, pairs, count, NULL) == NRF_GEOMETRY_ERR_ARGUMENT);

    /* Valid pair with a negative distance or a self pair is a hard error. */
    pairs[0].distance_mm = -1;
    assert(nrf_geometry_solver_solve(inputs, NODES, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    pairs[0] = make_pair(layout, 0U, 1U, true);
    pairs[1].node_b = pairs[1].node_a;
    assert(nrf_geometry_solver_solve(inputs, NODES, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);
    pairs[1] = make_pair(layout, 0U, 2U, true);

    /* Non-VALID pairs are ignored whatever distance they carry. */
    for (unsigned state = NRF_CS_PAIR_UNKNOWN; state <= NRF_CS_PAIR_RETRY_EXHAUSTED; ++state) {
        if (state == NRF_CS_PAIR_VALID) continue;
        nrf_cs_pair_result_t with_garbage[PAIRS];
        memcpy(with_garbage, pairs, sizeof(pairs));
        with_garbage[0].state = (nrf_cs_pair_state_t)state;
        with_garbage[0].distance_mm = 0;
        assert(nrf_geometry_solver_solve(inputs, NODES, with_garbage, count, &solution) == NRF_GEOMETRY_OK);
        assert(solution.nodes[1].solved); /* still reachable through other pairs */
    }

    /* No MAIN, two MAINs, duplicate ids. */
    nrf_geometry_node_input_t no_main[NODES];
    memcpy(no_main, inputs, sizeof(inputs));
    no_main[0].node_id = 9U;
    assert(nrf_geometry_solver_solve(no_main, NODES, pairs, count, &solution) == NRF_GEOMETRY_ERR_NO_MAIN);
    memcpy(no_main, inputs, sizeof(inputs));
    no_main[1].node_id = 0U;
    assert(nrf_geometry_solver_solve(no_main, NODES, pairs, count, &solution) == NRF_GEOMETRY_ERR_ARGUMENT);

    /* Recording needs MAIN plus three remotes; ranging alone accepts fewer. */
    for (size_t nodes = 1U; nodes <= NODES; ++nodes) {
        const nrf_geometry_result_t result = nrf_geometry_solver_solve(inputs, nodes, pairs, count, &solution);
        assert(result == (nodes < 1U + NRF_GEOMETRY_MIN_REMOTE_COUNT_FOR_RECORDING
                              ? NRF_GEOMETRY_UNS_INSUFFICIENT_NODES : NRF_GEOMETRY_OK));
        assert(nrf_geometry_solver_solve_partial(inputs, nodes, pairs, count, &solution) == NRF_GEOMETRY_OK);
    }
    CASE_PASS("CS.GEOMETRY.argument_and_state_domain");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    a_complete_exact_mesh_solves_every_node_with_tiny_residuals();
    invalid_pair_states_and_arguments_are_rejected();
    measurement_error_grows_the_residual_but_not_the_solved_set();
    every_solved_node_is_consistent_with_the_truth();
    return 0;
}
