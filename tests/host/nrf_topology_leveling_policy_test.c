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

int main(void)
{
    test_invalid_and_unpublished_state();
    test_counts_only_current_assigned_accepted_remotes();
    test_sensor_cache_validity_gates_acceptance_not_assignment();
    test_current_valid_but_not_accepted_states_do_not_count();
    return 0;
}
