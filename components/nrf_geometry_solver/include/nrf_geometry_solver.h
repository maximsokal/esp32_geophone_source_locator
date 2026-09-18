#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nrf_cs_scheduler.h"

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
} nrf_geometry_node_position_t;

#define NRF_GEOMETRY_MAX_NODES NRF_CS_MAX_NODES

typedef struct {
    size_t node_count;
    nrf_geometry_node_position_t nodes[NRF_GEOMETRY_MAX_NODES];
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
