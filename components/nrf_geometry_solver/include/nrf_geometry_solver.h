#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

#define NRF_GEOMETRY_SOLVER_VERSION "1"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform-neutral geometry solver (MAX-118): turns the Channel Sounding
 * pairwise distance mesh plus independently-known per-node relative
 * altitude (from BME280, MAX-114) into a relative 2D/3D layout.
 *
 * MAIN is always the fixed origin. Because CS gives distance only (no
 * angle-of-arrival), the resulting X/Y layout has the correct relative
 * shape but an ARBITRARY rotation and possible mirror-reflection -- there
 * is nothing in a distance-only measurement that fixes compass heading.
 * That is a fundamental limitation of this input, not a solver bug; the
 * caller must treat every solution as unanchored until a later GPS/
 * AprilTag step provides an absolute reference.
 */

typedef struct {
    nrf_cs_node_id_t node_id;
    int32_t z_mm;   /* relative altitude, 0 for MAIN, already subtracted */
} nrf_geometry_node_input_t;

typedef struct {
    nrf_cs_node_id_t node_id;
    bool solved;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
    /* Largest |solved 3D distance - measured CS distance| over every valid
     * pair between this node and another solved node (0 for an unsolved
     * node or one with no such pair). */
    int32_t residual_mm;
    /* MAX-144: 1-sigma horizontal position uncertainty from the refined
     * full-mesh covariance. 0 for MAIN (the fixed origin) and when no
     * refinement ran; INT32_MAX when the mesh does not determine the node
     * (singular/ill-conditioned normal equations). */
    int32_t uncertainty_mm;
} nrf_geometry_node_position_t;

#define NRF_GEOMETRY_MAX_NODES NRF_CS_MAX_NODES

/*
 * Distance-consistency tolerance (MAX-205), about 2.5x a typical single
 * Channel Sounding range error. Used for three decisions: a pair whose slant
 * distance is shorter than the nodes' height difference by more than this
 * is physically impossible and never used; two reference circles that miss
 * each other by at most this are treated as tangent (noise), by more as
 * inconsistent; and a pair whose solved distance disagrees with its
 * measurement by more than this counts as inconsistent.
 */
#define NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM 750

/*
 * MAX-144: 1-sigma range error used to weight a CS pair in the full-mesh
 * refinement: the pair's own sample spread (MAX-206) but never less than
 * this floor, which covers the per-pair bias that repeated samples cannot
 * reveal (to be replaced by the MAX-143 calibration).
 */
#define NRF_GEOMETRY_RANGE_SIGMA_FLOOR_MM 150

typedef struct {
    size_t node_count;
    nrf_geometry_node_position_t nodes[NRF_GEOMETRY_MAX_NODES];
    /* Max residual_mm over every solved node. */
    int32_t max_residual_mm;
    /* Valid pairs that are physically impossible (slant < |dz| - tolerance)
     * or, between two solved nodes, disagree with the solved layout by more
     * than NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM. */
    size_t inconsistent_pair_count;
    /* MAX-144: the weighted robust full-mesh refinement replaced the
     * sequential layout (false = sequential layout kept). */
    bool refined;
    uint8_t refine_iterations;
} nrf_geometry_solution_t;

typedef enum {
    NRF_GEOMETRY_OK = 0,
    NRF_GEOMETRY_ERR_ARGUMENT = -1,
    NRF_GEOMETRY_ERR_NO_MAIN = -2,
} nrf_geometry_result_t;

/*
 * Solve relative node positions. `inputs`/`input_count` must include every
 * node whose position is wanted, exactly one of which must have
 * node_id == 0 (MAIN, placed at the origin). `pairs`/`pair_count` is the
 * full CS pairwise mesh; only pairs with state == NRF_CS_PAIR_VALID and
 * both endpoints present in `inputs` are used -- callers must not have
 * already filtered pairs by state, this function does that check itself
 * (a MISSING/RETRY_EXHAUSTED pair's distance_mm is not a real 0mm reading).
 *
 * Nodes with no path of valid pairs back to MAIN through already-solved
 * nodes are reported with solved == false (x_mm/y_mm/z_mm left at 0), not
 * silently dropped -- the caller decides whether that blocks the gate.
 *
 * A node is also left unsolved (never placed by an arbitrary fallback) when
 * its position is not determined by its references (MAX-205):
 *   - only one usable reference once the layout's rotation is already fixed
 *     (the very first node placed after MAIN defines +X and is the only
 *     single-reference placement allowed);
 *   - exactly two references, a genuine mirror ambiguity, and the layout's
 *     reflection is already fixed (the first two-reference placement defines
 *     +Y and is the only one resolved by convention);
 *   - reference circles that miss each other by more than the tolerance, or
 *     a placement that disagrees with any reference by more than it.
 */
nrf_geometry_result_t nrf_geometry_solver_solve(
    const nrf_geometry_node_input_t *inputs,
    size_t input_count,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    nrf_geometry_solution_t *solution);

#ifdef __cplusplus
}
#endif
