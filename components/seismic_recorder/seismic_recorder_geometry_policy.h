#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_geometry_solver.h"
#include "nrf_topology.h"
#include "object_store.h"


/*
 * Per-channel status the impure caller gathers from nrf_topology_service's
 * sensor cache before calling into this pure module. `leveled` mirrors
 * NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED at the moment of the read, not a
 * live value -- matching nrf_topology_service_get_leveling_summary()'s own
 * definition of "accepted".
 */
typedef struct
{
    bool altitude_valid;
    int32_t altitude_mm;
    bool leveled;
} seismic_recorder_geometry_channel_status_t;


/*
 * Build the geometry solver's node list from the current topology snapshot
 * and per-channel altitude status. MAIN is always included at z_mm == 0 (the
 * fixed-origin convention, MAX-118 design decision -- not a sensor reading:
 * MAIN's own XIAO does not report SENSOR_REFRESH data, so every other node's
 * z_mm here is its raw absolute barometric altitude, not a MAIN-relative
 * delta. That carries a constant, unknown offset equal to MAIN's true
 * elevation versus the 101325 Pa sea-level reference -- harmless to
 * REMOTE-to-REMOTE relative Z, which is what source localization actually
 * needs, but MAIN's own Z in the solved layout is not to be trusted until a
 * later anchoring step. A REMOTE that is ASSIGNED but whose altitude is not
 * currently valid is left out of the input set entirely (and therefore
 * reported unsolved by the solver) rather than guessed at.
 *
 * Returns ESP_ERR_INVALID_STATE if MAIN itself is not ASSIGNED (the solver
 * always requires a MAIN node) and ESP_ERR_INVALID_ARG if more nodes would
 * be produced than `max_inputs` holds.
 */
esp_err_t seismic_recorder_geometry_build_inputs(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *input_count
);


/*
 * Map a solved geometry (keyed by CS node_id) back onto object_store's
 * pod-indexed schema (keyed by mux channel, pod_index == mux_channel for
 * channel < OBJECT_STORE_POD_COUNT -- NRF_CS_MAX_NODES caps a mesh at MAIN
 * plus 4 REMOTEs, the same 4 as OBJECT_STORE_POD_COUNT). `geometry` is fully
 * overwritten: `anchored` is always left false (MAX-118 design decision,
 * unchanged until GPS/AprilTag), and a pod is left `computed == false`
 * unless its topology-assigned node is present in `solution` and solved.
 */
void seismic_recorder_geometry_apply_solution(
    const nrf_geometry_solution_t *solution,
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    uint64_t computed_at_ms,
    object_store_geometry_t *geometry
);


/*
 * Whole-recording admission check: every currently ASSIGNED pod (channel <
 * OBJECT_STORE_POD_COUNT) must have ended up `computed == true` in
 * `geometry`, matching the "geometry known before AD7606 records" product
 * requirement. A REMOTE assigned on a channel >= OBJECT_STORE_POD_COUNT is
 * outside pod-mapped range and is not evaluated here -- NRF_CS_MAX_NODES
 * already blocks that mesh from completing a cycle at all.
 */
bool seismic_recorder_geometry_all_assigned_pods_computed(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry
);
