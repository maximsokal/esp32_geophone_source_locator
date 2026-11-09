#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nrf_cs_scheduler.h"
#include "nrf_geometry_solver.h"
#include "nrf_topology.h"
#include "nrf_topology_service.h"
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
    bool height_reference_valid;
    uint16_t height_reference_generation;
    int32_t relative_z_mm;
    /* The height relative to MAIN that the placement step settled (the centre of its 20 s window, good to about
     * 0.1 m). When present it is the Z of the geometry; the instantaneous relative_z_mm wanders by 10..20 cm. */
    bool placement_height_valid;
    int32_t placement_height_mm;
    bool leveled;
    uint64_t source_sample_uptime_ms;
    uint64_t esp_receipt_uptime_ms;
} seismic_recorder_geometry_channel_status_t;


typedef struct
{
    bool valid;
    uint16_t generation;
    int32_t relative_z_mm;
    /* MAIN's barometer reading of the moment: the air density that turns a pressure difference into metres
     * (nrf_baro_density_corrected_height_mm). Without it the linear 12 Pa/m height is used. */
    bool environment_valid;
    uint32_t pressure_pa;
    int32_t temperature_mc;
} seismic_recorder_geometry_main_reference_t;


/* Build solver inputs with a single generation-checked relative height policy. */
esp_err_t seismic_recorder_geometry_build_inputs(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    const seismic_recorder_geometry_main_reference_t *main_reference,
    seismic_recorder_geometry_z_policy_t z_policy,
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *input_count,
    seismic_recorder_geometry_z_provenance_t *provenance
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
    const seismic_recorder_geometry_z_provenance_t *provenance,
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


/*
 * Cable admission of a frozen ranging at START. `limits_mm` is per mux channel and every ASSIGNED REMOTE needs one
 * (ESP_ERR_INVALID_ARG otherwise).
 *  - MAIN assigned: a REMOTE is at most its cable (plus the measurement bin of an automatic distance) from MAIN, so a
 *    valid MAIN edge beyond it is refused, and after the solve so is a node whose radius from the origin exceeds it.
 *  - MAIN confirmed absent (REMOTE_ONLY): the origin is a REMOTE, not the housing, so there is no radius and no MAIN edge;
 *    a valid pair is refused when it exceeds the sum of its two cables (nrf_topology_pair_exceeds_cable_sum).
 * ESP_ERR_INVALID_STATE for a refusal; `bad_node` (may be NULL) names the node of a refused radius.
 */
esp_err_t seismic_recorder_geometry_check_cable_pairs(
    const nrf_topology_snapshot_t *topology,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT]
);

esp_err_t seismic_recorder_geometry_check_cable_radius(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_cs_node_id_t *bad_node
);


/*
 * REMOTE_ONLY registration: the position of each geophone in the registered scan, in the project frame (millimetres, +Z
 * up), taken from the centre of its AprilTag without any offset. Indexed by pod (= mux channel). `node_id` and `tag_id`
 * are the REMOTE's CS node id (= channel + 1) and the tag bound to it (G<k> <-> tag k). The scan is registered after the
 * recording, so the positions are bound to the frozen geometry they were made for by `geometry_computed_at_ms`.
 */
typedef struct
{
    bool valid;
    uint8_t node_id;
    uint8_t tag_id;
    int32_t x_mm;
    int32_t y_mm;
    int32_t z_mm;
} seismic_recorder_registered_position_t;

typedef struct
{
    bool available;
    uint64_t geometry_computed_at_ms;
    seismic_recorder_registered_position_t nodes[OBJECT_STORE_POD_COUNT];
} seismic_recorder_registered_positions_t;

/*
 * The localization geometry of a REMOTE_ONLY session: the Channel Sounding layout of `geometry` replaced by the registered
 * scan positions, all or nothing. It applies only to a session recorded in REMOTE_ONLY (`participation` says so) and only to
 * the very geometry the positions were made for. Every computed pod needs a valid position for its own node and tag,
 * positions must be finite and apart. On any refusal `geometry` is left untouched (the Channel Sounding layout stays):
 *  ESP_ERR_NOT_FOUND     nothing registered, or a computed pod has no valid position;
 *  ESP_ERR_INVALID_STATE not a REMOTE_ONLY session, positions made for another geometry, or a position that does not
 *                        belong to its pod;
 *  ESP_ERR_INVALID_ARG   NULL argument, or two positions closer than 10 mm.
 * On success the computed pods carry the scan coordinates, `uncertainty_mm` 0 (not determined: the tag-to-sensor offset is
 * accepted, not measured), `residual_mm` 0, their `computed_at_ms` is kept, and `geometry->anchored` is true.
 */
esp_err_t seismic_recorder_geometry_apply_registered_positions(
    const seismic_recorder_session_participation_t *participation,
    const seismic_recorder_registered_positions_t *registered,
    object_store_geometry_t *geometry
);
