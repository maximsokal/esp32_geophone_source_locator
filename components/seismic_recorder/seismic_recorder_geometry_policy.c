#include "seismic_recorder_geometry_policy.h"

#include <stdio.h>
#include <string.h>


#if SEISMIC_RECORDER_HARDWARE_ID_MAX != NRF_UPSTREAM_PROTOCOL_HW_ID_MAX
#error "seismic recorder hardware ID capacity must match the topology protocol"
#endif


static int32_t saturate_i32(int64_t value)
{
    if (value > (int64_t)INT32_MAX) {
        return INT32_MAX;
    }
    if (value < (int64_t)INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}


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

    if (topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED) {
        return ESP_ERR_INVALID_STATE;
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
    inputs[count].node_id = topology->main.node_id;
    inputs[count].z_mm = 0;
    ++count;

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
                : saturate_i32(
                      (int64_t)channels[channel].relative_z_mm -
                      (int64_t)main_reference->relative_z_mm
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
        pod->position_source = geometry->pods[i].computed
            ? provenance->effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
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
    return
        solution != NULL &&
        nrf_geometry_metrics_consistent(solution->inconsistent_pair_count, solution->max_residual_mm);
}
