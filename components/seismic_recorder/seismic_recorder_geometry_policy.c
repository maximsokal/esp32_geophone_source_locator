#include "seismic_recorder_geometry_policy.h"

#include <string.h>


esp_err_t seismic_recorder_geometry_build_inputs(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *input_count
)
{
    if (topology == NULL || channels == NULL || inputs == NULL || input_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (topology->main.state != NRF_TOPOLOGY_NODE_ASSIGNED) {
        return ESP_ERR_INVALID_STATE;
    }

    if (max_inputs < 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = 0U;
    inputs[count].node_id = topology->main.node_id;
    inputs[count].z_mm = 0;
    ++count;

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        if (!channels[channel].altitude_valid) {
            continue;
        }
        if (count >= max_inputs) {
            return ESP_ERR_INVALID_ARG;
        }
        inputs[count].node_id = topology->remotes[channel].node_id;
        inputs[count].z_mm = channels[channel].altitude_mm;
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
