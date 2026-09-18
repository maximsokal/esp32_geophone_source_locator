#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

    assert(nrf_geometry_solver_solve(inputs, 1U, NULL, 0U, &solution) ==
           NRF_GEOMETRY_OK);
    assert(solution.node_count == 1U);
    const nrf_geometry_node_position_t *main_node = find_node(&solution, 0U);
    assert(main_node != NULL);
    assert(main_node->solved);
    assert(main_node->x_mm == 0 && main_node->y_mm == 0);
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

    assert(nrf_geometry_solver_solve(inputs, 2U, pairs, 1U, &solution) ==
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
    assert(nrf_geometry_solver_solve(inputs, 2U, NULL, 0U, &solution) ==
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

    assert(nrf_geometry_solver_solve(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && !a->solved);
}

static int32_t iabs(int32_t value)
{
    return value < 0 ? -value : value;
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

    assert(nrf_geometry_solver_solve(inputs, 2U, pairs, 1U, &solution) ==
           NRF_GEOMETRY_OK);
    const nrf_geometry_node_position_t *a = find_node(&solution, 1U);
    assert(a != NULL && a->solved);
    assert(a->z_mm == 400);
    assert(iabs(a->x_mm - 300) <= 1);
}

int main(void)
{
    test_rejects_missing_main_and_bad_arguments();
    test_main_alone_is_the_origin();
    test_single_reference_places_along_arbitrary_heading();
    test_isolated_node_with_no_valid_pair_stays_unsolved();
    test_missing_pair_state_is_not_treated_as_zero_distance();
    test_trilateration_with_mirror_disambiguation();
    test_z_passes_through_unmodified();
    return 0;
}
