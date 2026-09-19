#include "unity.h"

#include "nrf_topology_service.h"

static void make_assigned_remote(
    nrf_topology_snapshot_t *topology,
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT],
    size_t channel,
    uint8_t node_id)
{
    topology->remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology->remotes[channel].node_id = node_id;
    topology->remotes[channel].mux_channel = (uint8_t)channel;
    cache[channel].identity = topology->remotes[channel];
    cache[channel].identity_valid = true;
    cache[channel].topology_current = true;
    cache[channel].response_valid = true;
    cache[channel].sensor.orientation_state = NRF_UPSTREAM_ORIENTATION_STATE_ACCEPTED;
}

TEST_CASE(
    "leveling summary public service counts assigned remotes and current accepted samples",
    "[nrf_topology_service]"
)
{
    nrf_topology_snapshot_t topology = {0};
    nrf_remote_sensor_cache_t cache[NRF_TOPOLOGY_REMOTE_COUNT] = {0};
    nrf_topology_leveling_summary_t summary = {99U, 99U};

    topology.generation = 7U;
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    make_assigned_remote(&topology, cache, 0U, 1U);
    make_assigned_remote(&topology, cache, 1U, 2U);
    cache[1U].topology_current = false;

    TEST_ASSERT_EQUAL(
        ESP_OK,
        nrf_topology_service_test_publish_state(true, &topology, true, cache)
    );
    TEST_ASSERT_EQUAL(ESP_OK, nrf_topology_service_get_leveling_summary(&summary));
    TEST_ASSERT_EQUAL_UINT32(2U, summary.assigned_count);
    TEST_ASSERT_EQUAL_UINT32(1U, summary.accepted_count);
}

TEST_CASE(
    "leveling summary public service rejects unpublished state and invalid injection",
    "[nrf_topology_service]"
)
{
    nrf_topology_leveling_summary_t summary = {0};
    nrf_topology_snapshot_t topology = {0};

    TEST_ASSERT_EQUAL(
        ESP_OK,
        nrf_topology_service_test_publish_state(false, NULL, false, NULL)
    );
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_STATE,
        nrf_topology_service_get_leveling_summary(&summary)
    );
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        nrf_topology_service_get_leveling_summary(NULL)
    );
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        nrf_topology_service_test_publish_state(true, NULL, false, NULL)
    );
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        nrf_topology_service_test_publish_state(false, &topology, true, NULL)
    );
}
