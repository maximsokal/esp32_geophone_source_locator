#include "nrf_topology_leveling_policy.h"

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
