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

/*
 * A current, valid sample that isn't ACCEPTED yet (UNAVAILABLE: no orientation
 * sample has arrived from this node yet; MOVING: gyro says it's not settled;
 * OUTSIDE: settled but past the tilt threshold) must still count toward
 * assigned_count but never accepted_count. Numeric values mirror
 * nrf_orientation_state_t (UNAVAILABLE=0, MOVING=1, ACCEPTED=2, OUTSIDE=3) per
 * nrf_upstream_protocol.h's own comment; only ACCEPTED has a named wire
 * constant there by design.
 */
static void test_current_valid_but_not_accepted_states_do_not_count(void)
{
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_topology_leveling_summary_t summary;
    const uint8_t non_accepted_states[] = {0U, 1U, 3U};

    for (size_t i = 0U; i < sizeof(non_accepted_states) / sizeof(non_accepted_states[0]); ++i) {
        memset(&topology, 0, sizeof(topology));
        memset(cache, 0, sizeof(cache));
        assign_remote(&topology, 5U);
        accept_cache(&cache[5]);
        cache[5].sensor.orientation_state = non_accepted_states[i];

        assert(nrf_topology_leveling_summary_compute(
                   true, &topology, true, cache, &summary) == ESP_OK);
        assert(summary.assigned_count == 1U);
        assert(summary.accepted_count == 0U);
    }
}

static void test_per_node_states_and_tilt(void)
{
    nrf_topology_snapshot_t topology;
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_topology_leveling_nodes_t nodes;
    memset(&topology, 0, sizeof(topology));
    memset(cache, 0, sizeof(cache));
    assign_remote(&topology, 1U);
    assign_remote(&topology, 4U);
    assign_remote(&topology, 6U);
    assign_remote(&topology, 7U);

    accept_cache(&cache[1]);
    cache[1].sensor.orientation_tilt_sin2_ppm = 0U;
    /* OUTSIDE by 30 degrees: sin^2(30) = 0.25. */
    accept_cache(&cache[4]);
    cache[4].sensor.orientation_state = 3U;
    cache[4].sensor.orientation_tilt_sin2_ppm = 250000U;
    /* 6: no response yet. 7: out-of-range wire value. */
    accept_cache(&cache[7]);
    cache[7].sensor.orientation_state = 9U;
    /* Unassigned channel with data is not listed. */
    accept_cache(&cache[2]);

    assert(nrf_topology_leveling_nodes_compute(true, &topology, true, cache, &nodes) == ESP_OK);
    assert(nodes.count == 4U);
    assert(nodes.nodes[0].channel == 1U && nodes.nodes[0].node_id == 2U);
    assert(nodes.nodes[0].state == NRF_TOPOLOGY_LEVELING_ACCEPTED);
    assert(nodes.nodes[0].tilt_valid && nodes.nodes[0].tilt_deg < 0.01f);
    assert(nodes.nodes[1].channel == 4U && nodes.nodes[1].state == NRF_TOPOLOGY_LEVELING_OUTSIDE);
    assert(nodes.nodes[1].tilt_deg > 29.9f && nodes.nodes[1].tilt_deg < 30.1f);
    assert(nodes.nodes[2].channel == 6U && nodes.nodes[2].state == NRF_TOPOLOGY_LEVELING_NO_DATA);
    assert(!nodes.nodes[2].tilt_valid);
    assert(nodes.nodes[3].state == NRF_TOPOLOGY_LEVELING_NO_DATA);

    /* UNAVAILABLE has no meaningful tilt. */
    cache[1].sensor.orientation_state = 0U;
    assert(nrf_topology_leveling_nodes_compute(true, &topology, true, cache, &nodes) == ESP_OK);
    assert(nodes.nodes[0].state == NRF_TOPOLOGY_LEVELING_UNAVAILABLE && !nodes.nodes[0].tilt_valid);

    /* Without a sensor cache every assigned node is NO_DATA. */
    assert(nrf_topology_leveling_nodes_compute(true, &topology, false, NULL, &nodes) == ESP_OK);
    assert(nodes.count == 4U && nodes.nodes[1].state == NRF_TOPOLOGY_LEVELING_NO_DATA);

    assert(nrf_topology_leveling_nodes_compute(false, &topology, true, cache, &nodes) == ESP_ERR_INVALID_STATE);
    assert(nodes.count == 0U);
    assert(nrf_topology_leveling_nodes_compute(true, &topology, true, cache, NULL) == ESP_ERR_INVALID_ARG);

    assert(nrf_topology_leveling_tilt_deg(1000000U) > 89.9f);
    assert(nrf_topology_leveling_tilt_deg(5000000U) > 89.9f); /* clamped */
    assert(strcmp(nrf_topology_leveling_node_state_name(NRF_TOPOLOGY_LEVELING_ACCEPTED), "level") == 0);
    assert(strcmp(nrf_topology_leveling_node_state_name(NRF_TOPOLOGY_LEVELING_OUTSIDE), "tilted") == 0);
}

int main(void)
{
    test_per_node_states_and_tilt();
    test_invalid_and_unpublished_state();
    test_counts_only_current_assigned_accepted_remotes();
    test_sensor_cache_validity_gates_acceptance_not_assignment();
    test_current_valid_but_not_accepted_states_do_not_count();
    return 0;
}
