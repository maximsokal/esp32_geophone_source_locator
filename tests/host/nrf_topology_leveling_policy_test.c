#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_topology_leveling_policy.h"

static void assign_remote(nrf_topology_snapshot_t *topology, uint8_t channel)
{
    topology->remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology->remotes[channel].node_id = (uint8_t)(channel + 1U);
    topology->remotes[channel].mux_channel = channel;
}

static void accept_cache(nrf_remote_sensor_cache_t *cache)
{
    cache->response_valid = true;
    cache->topology_current = true;
    cache->sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
}

static void test_invalid_and_unpublished_state(void)
{
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_topology_leveling_summary_t summary = {99U, 99U};
    memset(&topology, 0, sizeof(topology));
    memset(cache, 0, sizeof(cache));

    assert(nrf_topology_leveling_summary_compute(
               true, &topology, true, cache, NULL) == ESP_ERR_INVALID_ARG);
    assert(nrf_topology_leveling_summary_compute(
               false, &topology, true, cache, &summary) == ESP_ERR_INVALID_STATE);
    assert(summary.assigned_count == 0U && summary.accepted_count == 0U);
    assert(nrf_topology_leveling_summary_compute(
               true, NULL, false, NULL, &summary) == ESP_ERR_INVALID_ARG);
}

static void test_counts_only_current_assigned_accepted_remotes(void)
{
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_topology_leveling_summary_t summary;
    memset(&topology, 0, sizeof(topology));
    memset(cache, 0, sizeof(cache));

    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    assign_remote(&topology, 0U);
    assign_remote(&topology, 1U);
    assign_remote(&topology, 2U);

    accept_cache(&cache[0]);
    accept_cache(&cache[1]);
    cache[1].topology_current = false;
    cache[2].response_valid = false;

    /* An accepted cache on an unassigned channel must not be counted. */
    accept_cache(&cache[3]);

    assert(nrf_topology_leveling_summary_compute(
               true, &topology, true, cache, &summary) == ESP_OK);
    assert(summary.assigned_count == 3U);
    assert(summary.accepted_count == 1U);
}

static void test_sensor_cache_validity_gates_acceptance_not_assignment(void)
{
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_topology_leveling_summary_t summary;
    memset(&topology, 0, sizeof(topology));
    memset(cache, 0, sizeof(cache));
    assign_remote(&topology, 4U);
    accept_cache(&cache[4]);

    assert(nrf_topology_leveling_summary_compute(
               true, &topology, false, cache, &summary) == ESP_OK);
    assert(summary.assigned_count == 1U);
    assert(summary.accepted_count == 0U);
}

int main(void)
{
    test_invalid_and_unpublished_state();
    test_counts_only_current_assigned_accepted_remotes();
    test_sensor_cache_validity_gates_acceptance_not_assignment();
    return 0;
}
