#include "nrf_topology_leveling_policy.h"

#include <math.h>
#include <string.h>

esp_err_t nrf_topology_leveling_summary_compute(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_topology_leveling_summary_t *summary)
{
    if (summary == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary, 0, sizeof(*summary));
    if (!topology_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (topology == NULL || (sensor_cache_valid && cache == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }

        ++summary->assigned_count;
        if (sensor_cache_valid &&
            cache[channel].response_valid &&
            cache[channel].topology_current &&
            cache[channel].sensor.orientation_state ==
                NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED) {
            ++summary->accepted_count;
        }
    }

    return ESP_OK;
}


float nrf_topology_leveling_tilt_deg(uint32_t tilt_sin2_ppm)
{
    double sin2 = (double)tilt_sin2_ppm / 1000000.0;
    if (sin2 > 1.0) {
        sin2 = 1.0;
    }
    return (float)(asin(sqrt(sin2)) * 180.0 / 3.14159265358979323846);
}


esp_err_t nrf_topology_leveling_nodes_compute(
    bool topology_valid,
    const nrf_topology_snapshot_t *topology,
    bool sensor_cache_valid,
    const nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    nrf_topology_leveling_nodes_t *nodes)
{
    if (nodes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(nodes, 0, sizeof(*nodes));
    if (!topology_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (topology == NULL || (sensor_cache_valid && cache == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        if (topology->remotes[channel].state != NRF_TOPOLOGY_NODE_ASSIGNED) {
            continue;
        }
        nrf_topology_leveling_node_t *node = &nodes->nodes[nodes->count++];
        node->channel = channel;
        node->node_id = topology->remotes[channel].node_id;
        node->state = NRF_TOPOLOGY_LEVELING_NO_DATA;

        if (!sensor_cache_valid ||
            !cache[channel].response_valid ||
            !cache[channel].topology_current) {
            continue;
        }
        const uint8_t wire = cache[channel].sensor.orientation_state;
        if (wire > NRF_UPSTREAM_ORIENTATION_STATE_MAX) {
            continue; /* never trust an out-of-range value */
        }
        node->state = (nrf_topology_leveling_node_state_t)(wire + 1U);
        if (node->state != NRF_TOPOLOGY_LEVELING_UNAVAILABLE) {
            node->tilt_valid = true;
            node->tilt_deg = nrf_topology_leveling_tilt_deg(
                cache[channel].sensor.orientation_tilt_sin2_ppm);
        }
    }
    return ESP_OK;
}


const char *nrf_topology_leveling_node_state_name(nrf_topology_leveling_node_state_t state)
{
    switch (state) {
        case NRF_TOPOLOGY_LEVELING_UNAVAILABLE: return "unavailable";
        case NRF_TOPOLOGY_LEVELING_MOVING: return "moving";
        case NRF_TOPOLOGY_LEVELING_ACCEPTED: return "level";
        case NRF_TOPOLOGY_LEVELING_OUTSIDE: return "tilted";
        case NRF_TOPOLOGY_LEVELING_NO_DATA:
        default: return "no_data";
    }
}
