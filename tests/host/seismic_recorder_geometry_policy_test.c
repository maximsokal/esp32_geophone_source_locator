#include <assert.h>
#include <string.h>

#include "seismic_recorder_geometry_policy.h"


static void set_assigned(
    nrf_topology_snapshot_t *topology,
    uint8_t channel,
    uint8_t node_id
)
{
    topology->remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology->remotes[channel].node_id = node_id;
    topology->remotes[channel].mux_channel = channel;
}


static void test_build_inputs_requires_main(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;

    assert(
        seismic_recorder_geometry_build_inputs(&topology, channels, inputs, NRF_GEOMETRY_MAX_NODES, &count) ==
        ESP_ERR_INVALID_STATE
    );
}


static void test_build_inputs_skips_unassigned_and_invalid_altitude(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.main.node_id = 0U;
    set_assigned(&topology, 0U, 1U);
    set_assigned(&topology, 1U, 2U);
    /* channel 2 intentionally left unassigned. */

    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    channels[0].altitude_valid = true;
    channels[0].altitude_mm = 1500;
    channels[0].leveled = true;
    /* channel 1 is ASSIGNED but altitude not yet valid -- must be excluded. */
    channels[1].altitude_valid = false;

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        seismic_recorder_geometry_build_inputs(&topology, channels, inputs, NRF_GEOMETRY_MAX_NODES, &count) ==
        ESP_OK
    );
    assert(count == 2U);
    assert(inputs[0].node_id == 0U && inputs[0].z_mm == 0);
    assert(inputs[1].node_id == 1U && inputs[1].z_mm == 1500);
}


static void test_build_inputs_rejects_overflow(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    for (uint8_t channel = 0U; channel < NRF_TOPOLOGY_REMOTE_COUNT; ++channel) {
        set_assigned(&topology, channel, (uint8_t)(channel + 1U));
        channels[channel].altitude_valid = true;
        channels[channel].altitude_mm = 0;
    }

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    /* 1 MAIN + 8 REMOTEs overflows a 5-node buffer. */
    assert(
        seismic_recorder_geometry_build_inputs(&topology, channels, inputs, NRF_GEOMETRY_MAX_NODES, &count) ==
        ESP_ERR_INVALID_ARG
    );
}


static void test_apply_solution_maps_pods_and_skips_unsolved(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    set_assigned(&topology, 0U, 1U);
    set_assigned(&topology, 1U, 2U);
    /* Channel 1's node never made it into the solution -- must stay computed==false. */

    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    channels[0].leveled = true;
    channels[1].leveled = true;

    nrf_geometry_solution_t solution;
    memset(&solution, 0, sizeof(solution));
    solution.node_count = 1U;
    solution.nodes[0].node_id = 1U;
    solution.nodes[0].solved = true;
    solution.nodes[0].x_mm = 300;
    solution.nodes[0].y_mm = -40;
    solution.nodes[0].z_mm = 1500;

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    seismic_recorder_geometry_apply_solution(&solution, &topology, channels, 123456ULL, &geometry);

    assert(!geometry.anchored);
    assert(geometry.pods[0].computed);
    assert(geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 300 && geometry.pods[0].y_mm == -40 && geometry.pods[0].z_mm == 1500);
    assert(geometry.pods[0].computed_at_ms == 123456ULL);
    assert(!geometry.pods[1].computed);
    assert(!geometry.pods[2].computed);
    assert(!geometry.pods[3].computed);

    assert(!seismic_recorder_geometry_all_assigned_pods_computed(&topology, &geometry));

    /* Once channel 1 also solves, the whole-object gate opens. */
    solution.node_count = 2U;
    solution.nodes[1].node_id = 2U;
    solution.nodes[1].solved = true;
    seismic_recorder_geometry_apply_solution(&solution, &topology, channels, 123456ULL, &geometry);
    assert(seismic_recorder_geometry_all_assigned_pods_computed(&topology, &geometry));
}


int main(void)
{
    test_build_inputs_requires_main();
    test_build_inputs_skips_unassigned_and_invalid_altitude();
    test_build_inputs_rejects_overflow();
    test_apply_solution_maps_pods_and_skips_unsolved();
    return 0;
}
