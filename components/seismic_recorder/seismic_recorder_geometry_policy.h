#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_geometry_solver.h"
#include "nrf_topology.h"
#include "object_store.h"
#include "seismic_recorder.h"


/*
 * Per-channel status the impure caller gathers from nrf_topology_service's
 * sensor cache before calling into this pure module. `leveled` mirrors
 * NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED at the moment of the read, not a
 * live value -- matching nrf_topology_service_get_leveling_summary()'s own
 * definition of "accepted".
 */
typedef struct
{
    /* The REMOTE's sensor snapshot was valid and topology-current at read
     * time. Only such a REMOTE is handed to the solver at all. */
    bool sensor_valid;
    /* ABSOLUTE barometric altitude exactly as nrf_bme280_pressure_to_
     * altitude_mm() reports it: referenced to a fixed 101325 Pa sea-level
     * pressure (hundreds of meters away from 0 at a real site). Never a
     * solver z -- seismic_recorder_geometry_build_inputs() converts it into
     * a relative height, or ignores it, per the selected Z policy. */
    int32_t barometric_altitude_mm;
    bool leveled;
} seismic_recorder_geometry_channel_status_t;


/*
 * How per-node relative height (nrf_geometry_node_input_t.z_mm, "relative
 * altitude, 0 for MAIN, already subtracted") is derived (MAX-204).
 *
 * PLANAR: every node at z == 0. The default: BME280 absolute accuracy is
 * about +-1 hPa between separate units (~+-8 m of apparent height), so raw
 * barometric differences between different REMOTE boards are not a usable
 * height without per-device offset calibration. For a practically flat
 * deployment the error of ignoring a real height difference dz on a slant
 * CS distance d is only d - sqrt(d^2 - dz^2) (e.g. 0.1 m for dz = 1 m at
 * d = 5 m), far smaller than an uncalibrated barometric offset.
 *
 * BAROMETRIC_RELATIVE: z_i = barometric_altitude_i - median over every valid
 * REMOTE in this same read. MAIN reports no barometric data, so it is
 * placed on that same median reference plane (z == 0). Only differences
 * taken at the same moment are used, so weather drift cancels; per-device
 * offsets do NOT cancel -- use only with calibrated units.
 */
typedef enum
{
    SEISMIC_RECORDER_GEOMETRY_Z_PLANAR = 0,
    SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
} seismic_recorder_geometry_z_policy_t;


/*
 * Build the geometry solver's node list from the current topology snapshot
 * and per-channel sensor status. MAIN is always included at z_mm == 0 (the
 * fixed-origin convention). Every REMOTE z_mm is RELATIVE, derived per
 * `z_policy` (see seismic_recorder_geometry_z_policy_t) -- never the raw
 * absolute barometric altitude, which would otherwise put REMOTEs tens to
 * hundreds of meters above MAIN and collapse every MAIN-to-REMOTE horizontal
 * projection to zero (MAX-204). A REMOTE that is ASSIGNED but whose sensor
 * snapshot is not currently valid is left out of the input set entirely
 * (and therefore reported unsolved by the solver) rather than guessed at.
 *
 * Returns ESP_ERR_INVALID_STATE if MAIN itself is not ASSIGNED (the solver
 * always requires a MAIN node) and ESP_ERR_INVALID_ARG if more nodes would
 * be produced than `max_inputs` holds or `z_policy` is unknown.
 */
esp_err_t seismic_recorder_geometry_build_inputs(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    seismic_recorder_geometry_z_policy_t z_policy,
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


/* Build the frozen REMOTE participation and position-source snapshot. */
esp_err_t seismic_recorder_geometry_build_participation(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry,
    seismic_recorder_session_participation_t *participation
);


/*
 * MAX-148: map the CS scheduler's own pair results (whatever cycle.pairs the
 * impure caller read from nrf_topology_service_get_cs_cycle()) onto
 * object_store's decoupled, persistence-facing schema -- verbatim, no
 * reinterpretation of distance/confidence/attempts. `valid` mirrors
 * NRF_CS_PAIR_VALID exactly; every other state (including one this cycle
 * never reached, e.g. still PENDING) maps to valid == false rather than
 * being silently upgraded. Pairs beyond OBJECT_STORE_MAX_CS_PAIRS are
 * dropped (cannot happen in practice: NRF_CS_MAX_PAIRS ==
 * OBJECT_STORE_MAX_CS_PAIRS, both derived from the same 5-node mesh cap).
 */
void seismic_recorder_geometry_apply_cs_pairs(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    object_store_cs_pairs_t *cs_pairs_out
);


/*
 * MAX-205 admission check on the solve itself: false when any valid CS pair
 * is physically impossible or disagrees with the solved layout by more than
 * NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM. "Every pod computed" alone is not
 * enough -- a geometrically meaningless layout must not be frozen into a
 * recording session.
 */
bool seismic_recorder_geometry_solution_consistent(
    const nrf_geometry_solution_t *solution
);
