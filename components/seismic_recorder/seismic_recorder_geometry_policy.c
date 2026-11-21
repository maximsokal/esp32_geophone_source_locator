#include "seismic_recorder_geometry_policy.h"

#include <stdio.h>
#include <string.h>

#include "nrf_baro_altitude.h"


#if SEISMIC_RECORDER_HARDWARE_ID_MAX != NRF_UPSTREAM_PROTOCOL_HW_ID_MAX
#error "seismic recorder hardware ID capacity must match the topology protocol"
#endif


esp_err_t seismic_recorder_geometry_build_inputs(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    const seismic_recorder_geometry_main_reference_t *main_reference,
    seismic_recorder_geometry_z_policy_t z_policy,
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *input_count,
    seismic_recorder_geometry_z_provenance_t *provenance
)
{
    if (topology == NULL || channels == NULL || inputs == NULL || input_count == NULL ||
        provenance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        z_policy != SEISMIC_RECORDER_GEOMETRY_Z_PLANAR &&
        z_policy != SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    /* MAIN assigned: the legacy mesh. MAIN confirmed absent: the REMOTEs are the whole mesh (REMOTE_ONLY). Anything else is
     * not a topology a geometry may be built on. */
    const bool main_is_node = topology->main.state == NRF_TOPOLOGY_NODE_ASSIGNED;
    if (!main_is_node) {
        bool any_remote = false;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            any_remote = any_remote || topology->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED;
        }
        if (topology->main.state != NRF_TOPOLOGY_NODE_ABSENT || !any_remote) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (max_inputs < 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(provenance, 0, sizeof(*provenance));
    provenance->available = true;
    provenance->effective_policy = z_policy;
    if (z_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE) {
        if (main_reference == NULL || !main_reference->valid || main_reference->generation == 0U) {
            provenance->effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
            strcpy(provenance->fallback_reason, "main_reference_invalid");
        } else {
            for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
                if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
                    continue;
                }
                /* The caller checked the settled height against MAIN's reference generation. */
                if (channels[channel].placement_height_valid) {
                    continue;
                }
                if (!channels[channel].height_reference_valid ||
                    channels[channel].height_reference_generation == 0U) {
                    provenance->effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
                    strcpy(provenance->fallback_reason, "remote_reference_invalid");
                    break;
                }
                if (channels[channel].height_reference_generation != main_reference->generation) {
                    provenance->effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
                    strcpy(provenance->fallback_reason, "reference_generation_mismatch");
                    break;
                }
            }
        }
    }

    size_t count = 0U;
    if (main_is_node) {
        inputs[count].node_id = topology->main.node_id;
        inputs[count].z_mm = 0;
        ++count;
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        if (!channels[channel].sensor_valid) {
            continue;
        }
        if (count >= max_inputs) {
            return ESP_ERR_INVALID_ARG;
        }
        inputs[count].node_id = topology->remotes[channel].node_id;
        inputs[count].z_mm =
            provenance->effective_policy != SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
            ? 0
            : channels[channel].placement_height_valid
                ? channels[channel].placement_height_mm
                : nrf_baro_density_corrected_height_mm(
                      (int64_t)channels[channel].relative_z_mm -
                          (int64_t)main_reference->relative_z_mm,
                      main_reference->environment_valid,
                      main_reference->pressure_pa,
                      main_reference->temperature_mc
                  );
        ++count;
    }

    *input_count = count;
    return ESP_OK;
}


static const nrf_geometry_node_position_t *find_solved_node(
    const nrf_geometry_solution_t *solution,
    nrf_cs_node_id_t node_id
)
{
    for (size_t i = 0U; i < solution->node_count; ++i) {
        if (solution->nodes[i].node_id == node_id) {
            return &solution->nodes[i];
        }
    }
    return NULL;
}


void seismic_recorder_geometry_apply_solution(
    const nrf_geometry_solution_t *solution,
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    uint64_t computed_at_ms,
    object_store_geometry_t *geometry
)
{
    if (solution == NULL || topology == NULL || channels == NULL || geometry == NULL) {
        return;
    }

    memset(geometry, 0, sizeof(*geometry));
    geometry->anchored = false;

    for (size_t pod_index = 0U; pod_index < OBJECT_STORE_POD_COUNT; ++pod_index) {
        const uint8_t channel = (uint8_t)pod_index;
        if (channel >= NRF_TOPOLOGY_REMOTE_COUNT ||
            topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }

        const nrf_geometry_node_position_t *node =
            find_solved_node(solution, topology->remotes[channel].node_id);
        if (node == NULL || !node->solved) {
            continue;
        }

        object_store_pod_geometry_t *pod = &geometry->pods[pod_index];
        pod->computed = true;
        pod->leveled = channels[channel].leveled;
        pod->x_mm = node->x_mm;
        pod->y_mm = node->y_mm;
        pod->z_mm = node->z_mm;
        pod->residual_mm = node->residual_mm;
        pod->uncertainty_mm = node->uncertainty_mm;
        pod->computed_at_ms = computed_at_ms;
    }
}


bool seismic_recorder_geometry_all_assigned_pods_computed(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry
)
{
    if (topology == NULL || geometry == NULL) {
        return false;
    }

    for (size_t pod_index = 0U; pod_index < OBJECT_STORE_POD_COUNT; ++pod_index) {
        const uint8_t channel = (uint8_t)pod_index;
        if (channel >= NRF_TOPOLOGY_REMOTE_COUNT ||
            topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        if (!geometry->pods[pod_index].computed) {
            return false;
        }
    }

    return true;
}


esp_err_t seismic_recorder_geometry_build_participation(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry,
    const seismic_recorder_geometry_z_provenance_t *provenance,
    seismic_recorder_session_participation_t *participation
)
{
    if (topology == NULL || geometry == NULL || provenance == NULL || participation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(participation, 0, sizeof(*participation));
    participation->z_provenance = *provenance;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        const nrf_topology_node_snapshot_t *remote = &topology->remotes[i];
        seismic_recorder_pod_participation_t *pod = &participation->pods[i];
        pod->assigned = remote->state == NRF_TOPOLOGY_NODE_ASSIGNED;
        /* `anchored` is only ever set by the registered-positions step: the layout then is the scan, not Channel Sounding. */
        pod->position_source = geometry->pods[i].computed
            ? geometry->anchored
                ? SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX
                : provenance->effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
                    ? SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO
                    : SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING
            : SEISMIC_RECORDER_POSITION_SOURCE_NONE;

        if (!pod->assigned) {
            continue;
        }

        pod->node_id_known = true;
        pod->node_id = remote->node_id;
        pod->hardware_id_length = remote->hardware_id_length;
        if (pod->hardware_id_length > SEISMIC_RECORDER_HARDWARE_ID_MAX) {
            pod->hardware_id_length = SEISMIC_RECORDER_HARDWARE_ID_MAX;
        }
        memcpy(pod->hardware_id, remote->hardware_id, pod->hardware_id_length);
    }

    return ESP_OK;
}


void seismic_recorder_geometry_apply_cs_pairs(
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    object_store_cs_pairs_t *cs_pairs_out
)
{
    if (cs_pairs_out == NULL) {
        return;
    }

    memset(cs_pairs_out, 0, sizeof(*cs_pairs_out));
    if (pairs == NULL) {
        return;
    }

    size_t count = 0U;
    for (size_t i = 0U; i < pair_count && count < OBJECT_STORE_MAX_CS_PAIRS; ++i) {
        object_store_cs_pair_t *out = &cs_pairs_out->pairs[count];
        const char *state = "unknown";
        switch (pairs[i].state) {
            case NRF_CS_PAIR_PENDING: state = "pending"; break;
            case NRF_CS_PAIR_VALID: state = "valid"; break;
            case NRF_CS_PAIR_MISSING: state = "missing"; break;
            case NRF_CS_PAIR_RETRY_EXHAUSTED: state = "retry_exhausted"; break;
            case NRF_CS_PAIR_CABLE_EXCEEDED: state = "cable_exceeded"; break;
            case NRF_CS_PAIR_DIRECTIONS_DISAGREE: state = "directions_disagree"; break;
            case NRF_CS_PAIR_UNKNOWN:
            default: break;
        }
        const char *last_failure = "unknown";
        switch (pairs[i].last_failure) {
            case NRF_CS_FAILURE_NONE: last_failure = "none"; break;
            case NRF_CS_FAILURE_TIMEOUT: last_failure = "timeout"; break;
            case NRF_CS_FAILURE_TRANSPORT: last_failure = "transport"; break;
            case NRF_CS_FAILURE_OUTLIER: last_failure = "outlier"; break;
            case NRF_CS_FAILURE_DISCONNECTED: last_failure = "disconnected"; break;
            default: break;
        }
        out->node_a = pairs[i].node_a;
        out->node_b = pairs[i].node_b;
        out->valid = pairs[i].state == NRF_CS_PAIR_VALID;
        out->distance_mm = pairs[i].distance_mm;
        out->confidence = pairs[i].confidence;
        out->attempts = pairs[i].attempts;
        out->valid_samples = pairs[i].valid_samples;
        out->spread_mm = pairs[i].spread_mm;
        out->rejected_samples = pairs[i].rejected_samples;
        snprintf(out->state, sizeof(out->state), "%s", state);
        snprintf(out->last_failure, sizeof(out->last_failure), "%s", last_failure);
        out->provider_quality_flags = pairs[i].provider_quality_flags;
        out->scheduler_quality_flags = pairs[i].scheduler_quality_flags;
        out->automatic_distance_mm = pairs[i].automatic_distance_mm;
        ++count;
    }
    cs_pairs_out->pair_count = count;
}


bool seismic_recorder_geometry_solution_consistent(
    const nrf_geometry_solution_t *solution
)
{
    if (solution == NULL || solution->node_count > NRF_GEOMETRY_MAX_NODES ||
        !nrf_geometry_metrics_consistent(solution->inconsistent_pair_count, solution->max_residual_mm)) return false;
    for (size_t i = 0U; i < solution->node_count; ++i) {
        if (solution->nodes[i].solved &&
            !nrf_geometry_position_uncertainty_acceptable(solution->nodes[i].uncertainty_mm)) return false;
    }
    return true;
}


static const nrf_topology_node_snapshot_t *remote_of_node(const nrf_topology_snapshot_t *topology, nrf_cs_node_id_t node)
{
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED && topology->remotes[channel].node_id == node) {
            return &topology->remotes[channel];
        }
    }
    return NULL;
}

static uint32_t limit_of_node(
    const nrf_topology_snapshot_t *topology, const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT], nrf_cs_node_id_t node)
{
    const nrf_topology_node_snapshot_t *remote = remote_of_node(topology, node);
    return remote != NULL && remote->mux_channel < NRF_TOPOLOGY_REMOTE_COUNT ? limits_mm[remote->mux_channel] : 0U;
}


esp_err_t seismic_recorder_geometry_check_cable_pairs(
    const nrf_topology_snapshot_t *topology,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT]
)
{
    if (topology == NULL || limits_mm == NULL || (pairs == NULL && pair_count != 0U)) return ESP_ERR_INVALID_ARG;
    const bool with_main = topology->main.state == NRF_TOPOLOGY_NODE_ASSIGNED;
    if (!with_main && topology->main.state != NRF_TOPOLOGY_NODE_ABSENT) return ESP_ERR_INVALID_STATE;

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        if (limits_mm[channel] == 0U) return ESP_ERR_INVALID_ARG;
        if (!with_main) continue;
        for (size_t i = 0U; i < pair_count; ++i) {
            const nrf_cs_pair_result_t *pair = &pairs[i];
            if (pair->state != NRF_CS_PAIR_VALID) continue;
            const nrf_cs_node_id_t node = topology->remotes[channel].node_id;
            if (((pair->node_a == topology->main.node_id && pair->node_b == node) ||
                 (pair->node_b == topology->main.node_id && pair->node_a == node)) &&
                pair->distance_mm > 0 &&
                nrf_topology_pair_exceeds_cable(pair->distance_mm, limits_mm[channel], pair->scheduler_quality_flags)) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (with_main) return ESP_OK;

    /* No MAIN: a valid pair is bounded by the two cables together. */
    for (size_t i = 0U; i < pair_count; ++i) {
        const nrf_cs_pair_result_t *pair = &pairs[i];
        if (pair->state != NRF_CS_PAIR_VALID || pair->distance_mm <= 0) continue;
        if (nrf_topology_pair_exceeds_cable_sum(
                pair->distance_mm, limit_of_node(topology, limits_mm, pair->node_a),
                limit_of_node(topology, limits_mm, pair->node_b), pair->scheduler_quality_flags)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_geometry_check_cable_radius(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry,
    const nrf_cs_pair_result_t *pairs,
    size_t pair_count,
    const uint32_t limits_mm[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_cs_node_id_t *bad_node
)
{
    if (topology == NULL || geometry == NULL || limits_mm == NULL || (pairs == NULL && pair_count != 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (topology->main.state == NRF_TOPOLOGY_NODE_ABSENT) return ESP_OK; /* the origin is a REMOTE: no radius from the housing */
    if (topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED) return ESP_ERR_INVALID_STATE;

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT && channel < OBJECT_STORE_POD_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) continue;
        const object_store_pod_geometry_t *pod = &geometry->pods[channel];
        const double radius2 = (double)pod->x_mm * pod->x_mm +
            (double)pod->y_mm * pod->y_mm + (double)pod->z_mm * pod->z_mm;
        /* Match ranging: one mm for rounded XYZ plus what the node's own measured MAIN edge exceeded the cable by
         * (absent MAIN edges get the rounding millimetre only). */
        const double cable_bound_mm = (double)limits_mm[channel] + (double)nrf_topology_cable_radius_allowance_mm(
            pairs, pair_count, topology->main.node_id, topology->remotes[channel].node_id, limits_mm[channel]);
        if (radius2 > cable_bound_mm * cable_bound_mm) {
            if (bad_node != NULL) *bad_node = topology->remotes[channel].node_id;
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}


esp_err_t seismic_recorder_geometry_apply_registered_positions(
    const seismic_recorder_session_participation_t *participation,
    const seismic_recorder_registered_positions_t *registered,
    object_store_geometry_t *geometry
)
{
    if (participation == NULL || registered == NULL || geometry == NULL) return ESP_ERR_INVALID_ARG;
    if (!registered->available) return ESP_ERR_NOT_FOUND;
    /* Only a session recorded in REMOTE_ONLY: with MAIN present (or a session older than the field) the Channel Sounding
     * layout is the geometry, as before. */
    const seismic_recorder_geometry_provenance_t *provenance = &participation->geometry_provenance;
    if (!provenance->available || !provenance->anchor_known || strcmp(provenance->topology_mode, "remote_only") != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The positions were made for one frozen geometry; a recomputed one is another layout. */
    uint64_t computed_at_ms = 0U;
    bool any_computed = false;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (geometry->pods[i].computed) {
            if (geometry->pods[i].computed_at_ms > computed_at_ms) computed_at_ms = geometry->pods[i].computed_at_ms;
            any_computed = true;
        }
    }
    if (!any_computed || registered->geometry_computed_at_ms != computed_at_ms) return ESP_ERR_INVALID_STATE;

    size_t computed = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (!geometry->pods[i].computed) continue;
        const seismic_recorder_registered_position_t *position = &registered->nodes[i];
        if (!position->valid) return ESP_ERR_NOT_FOUND;
        /* The position must be this pod's own: its node id, and the tag bound to that node (G<k> <-> tag k). */
        if (!participation->pods[i].assigned || !participation->pods[i].node_id_known ||
            position->node_id != participation->pods[i].node_id || position->tag_id != participation->pods[i].node_id) {
            return ESP_ERR_INVALID_STATE;
        }
        ++computed;
    }
    if (computed < 2U) return ESP_ERR_NOT_FOUND;

    /* Two geophones cannot share a place: such a pair is a mix-up of tags, not a layout. */
    for (size_t a = 0U; a < OBJECT_STORE_POD_COUNT; ++a) {
        if (!geometry->pods[a].computed) continue;
        for (size_t b = a + 1U; b < OBJECT_STORE_POD_COUNT; ++b) {
            if (!geometry->pods[b].computed) continue;
            const double dx = (double)registered->nodes[a].x_mm - registered->nodes[b].x_mm;
            const double dy = (double)registered->nodes[a].y_mm - registered->nodes[b].y_mm;
            const double dz = (double)registered->nodes[a].z_mm - registered->nodes[b].z_mm;
            if (dx * dx + dy * dy + dz * dz < 100.0) return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (!geometry->pods[i].computed) continue;
        object_store_pod_geometry_t *pod = &geometry->pods[i];
        pod->x_mm = registered->nodes[i].x_mm; /* `leveled` and `computed_at_ms` stay: the sensor's state, the geometry's identity */
        pod->y_mm = registered->nodes[i].y_mm;
        pod->z_mm = registered->nodes[i].z_mm;
        pod->residual_mm = 0;
        pod->uncertainty_mm = 0; /* not determined: the tag-to-sensor offset is accepted, not measured */
    }
    geometry->anchored = true;
    return ESP_OK;
}


esp_err_t seismic_recorder_geometry_apply_tag_layout(
    const seismic_recorder_session_participation_t *participation,
    const seismic_recorder_tag_layout_t *layout,
    const object_store_geometry_t *cs_geometry,
    object_store_geometry_t *out,
    size_t *remote_count_out
)
{
    if (participation == NULL || layout == NULL || cs_geometry == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (remote_count_out != NULL) *remote_count_out = 0U;
    if (!layout->available || !layout->nodes[0].valid) return ESP_ERR_NOT_FOUND;
    /* Issue #35: a degenerate (collinear / flat) layout is not geometry; the Channel Sounding layout stays the answer. */
    if (layout->weakest_main_triangle_height_mm < SEISMIC_RECORDER_TAG_LAYOUT_MIN_TRIANGLE_HEIGHT_MM) return ESP_ERR_NOT_FOUND;

    uint64_t identity_ms = 0U;
    size_t cs_computed = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        if (!cs_geometry->pods[i].computed) continue;
        ++cs_computed;
        if (cs_geometry->pods[i].computed_at_ms > identity_ms) identity_ms = cs_geometry->pods[i].computed_at_ms;
    }

    object_store_geometry_t candidate = *cs_geometry;
    size_t tagged = 0U;
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        const unsigned node = (unsigned)(i + 1U);
        const seismic_recorder_tag_node_t *tag = &layout->nodes[node];
        const seismic_recorder_pod_participation_t *pod_info = &participation->pods[i];
        /* A tag that was never actually observed (no sightings, no views, zero uncertainty) is not a measured position. */
        const bool measured = tag->observations >= 1U && tag->views >= 1U && tag->uncertainty_mm >= 1;
        const bool belongs = tag->valid && measured && tag->node_id == node && tag->tag_id == node &&
            pod_info->assigned && pod_info->node_id_known && pod_info->node_id == node;
        object_store_pod_geometry_t *pod = &candidate.pods[i];
        if (!belongs) {
            /* No tag for this remote: it does not take part in a tag-layout computation (no mixing with Channel Sounding). */
            const bool leveled = pod->leveled;
            memset(pod, 0, sizeof(*pod));
            pod->leveled = leveled;
            continue;
        }
        pod->computed = true;
        pod->x_mm = tag->x_mm;
        pod->y_mm = tag->y_mm;
        pod->z_mm = tag->z_mm;
        pod->residual_mm = 0;
        pod->uncertainty_mm = tag->uncertainty_mm;
        pod->computed_at_ms = identity_ms;
        ++tagged;
    }
    if (tagged < SEISMIC_RECORDER_TAG_LAYOUT_MIN_REMOTES) return ESP_ERR_NOT_FOUND;
    if (tagged < cs_computed) return ESP_ERR_INVALID_SIZE;

    /* Two sensors cannot share a place (MAIN is the origin and a sensor too): that is a mix-up of tags, not a layout. */
    for (size_t a = 0U; a < OBJECT_STORE_POD_COUNT; ++a) {
        if (!candidate.pods[a].computed) continue;
        const double ox = candidate.pods[a].x_mm, oy = candidate.pods[a].y_mm, oz = candidate.pods[a].z_mm;
        if (ox * ox + oy * oy + oz * oz < 100.0) return ESP_ERR_INVALID_ARG;
        for (size_t b = a + 1U; b < OBJECT_STORE_POD_COUNT; ++b) {
            if (!candidate.pods[b].computed) continue;
            const double dx = ox - candidate.pods[b].x_mm;
            const double dy = oy - candidate.pods[b].y_mm;
            const double dz = oz - candidate.pods[b].z_mm;
            if (dx * dx + dy * dy + dz * dz < 100.0) return ESP_ERR_INVALID_ARG;
        }
    }

    candidate.anchored = true;
    *out = candidate;
    if (remote_count_out != NULL) *remote_count_out = tagged;
    return ESP_OK;
}
