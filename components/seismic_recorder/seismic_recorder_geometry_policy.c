#include "seismic_recorder_geometry_policy.h"

#include <string.h>


/* Median of a small (<= NRF_TOPOLOGY_REMOTE_COUNT) set, sorted in place.
 * Even counts average the two middle values in 64-bit so the sum of two
 * large int32 altitudes cannot overflow. */
static int32_t median_altitude_mm(int32_t *values, size_t count)
{
    for (size_t i = 1U; i < count; ++i) {
        const int32_t key = values[i];
        size_t j = i;
        while (j > 0U && values[j - 1U] > key) {
            values[j] = values[j - 1U];
            --j;
        }
        values[j] = key;
    }

    if ((count % 2U) == 1U) {
        return values[count / 2U];
    }
    const int64_t sum = (int64_t)values[count / 2U - 1U] + (int64_t)values[count / 2U];
    return (int32_t)(sum / 2);
}


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
    seismic_recorder_geometry_z_policy_t z_policy,
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *input_count
)
{
    if (topology == NULL || channels == NULL || inputs == NULL || input_count == NULL) {
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

    /* Reference plane for BAROMETRIC_RELATIVE: median over every REMOTE that
     * will actually be handed to the solver, read at the same moment. */
    int32_t reference_altitude_mm = 0;
    if (z_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE) {
        int32_t altitudes[NRF_TOPOLOGY_REMOTE_COUNT];
        size_t altitude_count = 0U;
        for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
            if (
                topology->remotes[channel].state == NRF_TOPOLOGY_NODE_ASSIGNED &&
                channels[channel].sensor_valid
            ) {
                altitudes[altitude_count++] = channels[channel].barometric_altitude_mm;
            }
        }
        if (altitude_count > 0U) {
            reference_altitude_mm = median_altitude_mm(altitudes, altitude_count);
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
            z_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE
            ? saturate_i32(
                  (int64_t)channels[channel].barometric_altitude_mm -
                  (int64_t)reference_altitude_mm
              )
            : 0;
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
        out->node_a = pairs[i].node_a;
        out->node_b = pairs[i].node_b;
        out->valid = pairs[i].state == NRF_CS_PAIR_VALID;
        out->distance_mm = pairs[i].distance_mm;
        out->confidence = pairs[i].confidence;
        out->attempts = pairs[i].attempts;
        out->valid_samples = pairs[i].valid_samples;
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
        solution->inconsistent_pair_count == 0U &&
        solution->max_residual_mm <= NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM;
}
