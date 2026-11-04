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


static esp_err_t build_inputs_default(
    const nrf_topology_snapshot_t *topology,
    const seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    seismic_recorder_geometry_z_policy_t policy,
    nrf_geometry_node_input_t *inputs,
    size_t max_inputs,
    size_t *count
)
{
    const seismic_recorder_geometry_main_reference_t main_reference = {
        .valid = true,
        .generation = 1U,
        .relative_z_mm = 0,
    };
    seismic_recorder_geometry_z_provenance_t provenance;
    return seismic_recorder_geometry_build_inputs(
        topology, channels, &main_reference, policy, inputs, max_inputs, count, &provenance
    );
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
        build_inputs_default(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, inputs, NRF_GEOMETRY_MAX_NODES, &count) ==
        ESP_ERR_INVALID_STATE
    );
}


static void test_build_inputs_skips_unassigned_and_invalid_sensor_snapshot(void)
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
    channels[0].sensor_valid = true;
    channels[0].height_reference_valid = true;
    channels[0].height_reference_generation = 1U;
    channels[0].relative_z_mm = 0;
    channels[0].leveled = true;
    /* channel 1 is ASSIGNED but its sensor snapshot is not valid -- must be excluded. */
    channels[1].sensor_valid = false;

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        build_inputs_default(
            &topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
            inputs, NRF_GEOMETRY_MAX_NODES, &count
        ) == ESP_OK
    );
    assert(count == 2U);
    assert(inputs[0].node_id == 0U && inputs[0].z_mm == 0);
    /* The valid REMOTE has the same reference-relative Z as MAIN. */
    assert(inputs[1].node_id == 1U && inputs[1].z_mm == 0);
}


static void test_build_inputs_prefers_the_height_the_placement_settled(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.main.node_id = 0U;
    set_assigned(&topology, 0U, 1U);
    set_assigned(&topology, 1U, 2U);
    set_assigned(&topology, 2U, 3U);

    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        channels[channel].sensor_valid = true;
        channels[channel].height_reference_valid = true;
        channels[channel].height_reference_generation = 1U;
        channels[channel].leveled = true;
    }
    /* The instantaneous readings wander: 450, -300, 100 mm against MAIN's 100 mm (MAIN reference 1, z = 100). */
    channels[0].relative_z_mm = 550;
    channels[1].relative_z_mm = -200;
    channels[2].relative_z_mm = 200;
    /* The placement settled channel 0 at 120 mm and channel 1 at -250 mm; channel 2 has nothing settled. */
    channels[0].placement_height_valid = true;
    channels[0].placement_height_mm = 120;
    channels[1].placement_height_valid = true;
    channels[1].placement_height_mm = -250;

    const seismic_recorder_geometry_main_reference_t main_reference = {
        .valid = true, .generation = 1U, .relative_z_mm = 100,
    };
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    seismic_recorder_geometry_z_provenance_t provenance;
    assert(seismic_recorder_geometry_build_inputs(
        &topology, channels, &main_reference, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
        inputs, NRF_GEOMETRY_MAX_NODES, &count, &provenance) == ESP_OK);
    assert(count == 4U);
    assert(provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE);
    assert(inputs[1].node_id == 1U && inputs[1].z_mm == 120);    /* settled, not 550 - 100 = 450 */
    assert(inputs[2].node_id == 2U && inputs[2].z_mm == -250);   /* settled, not -200 - 100 = -300 */
    assert(inputs[3].node_id == 3U && inputs[3].z_mm == 100);    /* nothing settled: the instantaneous reading */

    /* A planar policy ignores every height, including the settled ones. */
    assert(seismic_recorder_geometry_build_inputs(
        &topology, channels, &main_reference, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR,
        inputs, NRF_GEOMETRY_MAX_NODES, &count, &provenance) == ESP_OK);
    assert(inputs[1].z_mm == 0 && inputs[2].z_mm == 0 && inputs[3].z_mm == 0);

    /* A missing reference generation falls back to planar before any height is looked at. */
    channels[1].height_reference_generation = 2U;
    assert(seismic_recorder_geometry_build_inputs(
        &topology, channels, &main_reference, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
        inputs, NRF_GEOMETRY_MAX_NODES, &count, &provenance) == ESP_OK);
    assert(provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_PLANAR);
    assert(inputs[1].z_mm == 0 && inputs[2].z_mm == 0);
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
        channels[channel].sensor_valid = true;
        channels[channel].height_reference_valid = true;
        channels[channel].height_reference_generation = 1U;
    }

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    /* 1 MAIN + 8 REMOTEs overflows a 5-node buffer. */
    assert(
        build_inputs_default(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, inputs, NRF_GEOMETRY_MAX_NODES, &count) ==
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


static void setup_three_remotes_with_relative_references(
    nrf_topology_snapshot_t *topology,
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT]
)
{
    memset(topology, 0, sizeof(*topology));
    topology->main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology->main.node_id = 0U;
    memset(channels, 0, sizeof(*channels) * NRF_TOPOLOGY_REMOTE_COUNT);
    const int32_t relative_z_mm[3] = { 100, 600, -400 };
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        set_assigned(topology, channel, (uint8_t)(channel + 1U));
        channels[channel].sensor_valid = true;
        channels[channel].height_reference_valid = true;
        channels[channel].height_reference_generation = 7U;
        channels[channel].relative_z_mm = relative_z_mm[channel];
    }
}


static void test_planar_policy_keeps_all_nodes_at_zero_z(void)
{
    nrf_topology_snapshot_t topology;
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    setup_three_remotes_with_relative_references(&topology, channels);

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        build_inputs_default(
            &topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR,
            inputs, NRF_GEOMETRY_MAX_NODES, &count
        ) == ESP_OK
    );
    assert(count == 4U);
    for (size_t i = 0U; i < count; ++i) {
        assert(inputs[i].z_mm == 0);
    }
}


static void test_barometric_relative_policy_subtracts_main_reference(void)
{
    nrf_topology_snapshot_t topology;
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    setup_three_remotes_with_relative_references(&topology, channels);
    const seismic_recorder_geometry_main_reference_t main_reference = {
        .valid = true, .generation = 7U, .relative_z_mm = 100,
    };
    seismic_recorder_geometry_z_provenance_t provenance;

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        seismic_recorder_geometry_build_inputs(
            &topology, channels, &main_reference, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
            inputs, NRF_GEOMETRY_MAX_NODES, &count, &provenance
        ) == ESP_OK
    );
    assert(count == 4U);
    assert(provenance.available);
    assert(provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE);
    assert(provenance.fallback_reason[0] == '\0');
    assert(inputs[0].z_mm == 0);
    assert(inputs[1].z_mm == 0);
    assert(inputs[2].z_mm == 500);
    assert(inputs[3].z_mm == -500);
    for (size_t i = 0U; i < count; ++i) {
        assert(inputs[i].z_mm > -10000 && inputs[i].z_mm < 10000);
    }
}


static void assert_planar_fallback(
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
    seismic_recorder_geometry_main_reference_t main_reference,
    const char *expected_reason
)
{
    nrf_topology_snapshot_t topology = {0};
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.main.node_id = 0U;
    for (uint8_t channel = 0U; channel < 3U; ++channel) {
        set_assigned(&topology, channel, (uint8_t)(channel + 1U));
    }
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    seismic_recorder_geometry_z_provenance_t provenance;
    assert(seismic_recorder_geometry_build_inputs(
        &topology, channels, &main_reference, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE,
        inputs, NRF_GEOMETRY_MAX_NODES, &count, &provenance
    ) == ESP_OK);
    assert(provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_PLANAR);
    assert(strcmp(provenance.fallback_reason, expected_reason) == 0);
    for (size_t i = 0U; i < count; ++i) assert(inputs[i].z_mm == 0);
}


static void test_invalid_references_fall_back_for_whole_recording(void)
{
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT] = {0};
    nrf_topology_snapshot_t topology;
    setup_three_remotes_with_relative_references(&topology, channels);
    assert_planar_fallback(channels, (seismic_recorder_geometry_main_reference_t){0}, "main_reference_invalid");

    setup_three_remotes_with_relative_references(&topology, channels);
    seismic_recorder_geometry_main_reference_t main_reference = {
        .valid = true, .generation = 7U, .relative_z_mm = 0,
    };
    channels[1].height_reference_valid = false;
    assert_planar_fallback(channels, main_reference, "remote_reference_invalid");

    setup_three_remotes_with_relative_references(&topology, channels);
    channels[1].height_reference_generation = 0U;
    assert_planar_fallback(channels, main_reference, "remote_reference_invalid");

    setup_three_remotes_with_relative_references(&topology, channels);
    channels[2].height_reference_generation = 8U;
    assert_planar_fallback(channels, main_reference, "reference_generation_mismatch");
}


static void test_unknown_z_policy_is_rejected(void)
{
    nrf_topology_snapshot_t topology;
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    setup_three_remotes_with_relative_references(&topology, channels);

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        build_inputs_default(
            &topology, channels, (seismic_recorder_geometry_z_policy_t)99,
            inputs, NRF_GEOMETRY_MAX_NODES, &count
        ) == ESP_ERR_INVALID_ARG
    );
}


/* End-to-end with the real solver: a 4 m MAIN-to-REMOTE slant distance must
 * produce a non-degenerate layout, not every pod collapsed onto (0,0). */
static void test_realistic_altitude_does_not_collapse_layout(void)
{
    nrf_topology_snapshot_t topology;
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    setup_three_remotes_with_relative_references(&topology, channels);

    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    assert(
        build_inputs_default(
            &topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR,
            inputs, NRF_GEOMETRY_MAX_NODES, &count
        ) == ESP_OK
    );

    /* MAIN(0) at origin, node 1 at (4,0), node 2 at (0,4), node 3 at (4,4) m. */
    nrf_cs_pair_result_t pairs[6];
    memset(pairs, 0, sizeof(pairs));
    const struct { uint8_t a; uint8_t b; int32_t d; } mesh[6] = {
        { 0U, 1U, 4000 }, { 0U, 2U, 4000 }, { 0U, 3U, 5657 },
        { 1U, 2U, 5657 }, { 1U, 3U, 4000 }, { 2U, 3U, 4000 },
    };
    for (size_t i = 0U; i < 6U; ++i) {
        pairs[i].node_a = mesh[i].a;
        pairs[i].node_b = mesh[i].b;
        pairs[i].distance_mm = mesh[i].d;
        pairs[i].state = NRF_CS_PAIR_VALID;
        pairs[i].confidence = 1000U;
    }

    nrf_geometry_solution_t solution;
    assert(nrf_geometry_solver_solve(inputs, count, pairs, 6U, &solution) == NRF_GEOMETRY_OK);

    size_t at_origin = 0U;
    for (size_t i = 0U; i < solution.node_count; ++i) {
        assert(solution.nodes[i].solved);
        if (solution.nodes[i].x_mm == 0 && solution.nodes[i].y_mm == 0) {
            ++at_origin;
        }
    }
    assert(at_origin == 1U); /* only MAIN */
}


/* MAX-205: "every pod computed" is not enough -- an inconsistent solve must
 * not open the recording gate, and the residual must reach the pod record. */
static void test_solution_consistency_gate_and_residual_mapping(void)
{
    nrf_geometry_solution_t solution;
    memset(&solution, 0, sizeof(solution));
    assert(!seismic_recorder_geometry_solution_consistent(NULL));
    assert(seismic_recorder_geometry_solution_consistent(&solution));

    solution.max_residual_mm = NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM;
    assert(seismic_recorder_geometry_solution_consistent(&solution));
    solution.max_residual_mm = NRF_GEOMETRY_CONSISTENCY_TOLERANCE_MM + 1;
    assert(!seismic_recorder_geometry_solution_consistent(&solution));
    solution.max_residual_mm = 0;
    solution.inconsistent_pair_count = 1U;
    assert(!seismic_recorder_geometry_solution_consistent(&solution));

    const int32_t residuals[] = {-1, 0, 750, 751, INT32_MAX};
    for (size_t count = 0U; count < 2U; ++count) {
        for (size_t i = 0U; i < sizeof(residuals) / sizeof(residuals[0]); ++i) {
            solution.inconsistent_pair_count = count;
            solution.max_residual_mm = residuals[i];
            assert(seismic_recorder_geometry_solution_consistent(&solution) ==
                nrf_geometry_metrics_consistent(count, residuals[i]));
        }
    }

    memset(&solution, 0, sizeof(solution));
    solution.node_count = 1U;
    solution.nodes[0].solved = true;
    const int32_t uncertainties[] = {-1, 0, 750, 751, INT32_MAX};
    for (size_t i = 0U; i < sizeof(uncertainties) / sizeof(uncertainties[0]); ++i) {
        solution.nodes[0].uncertainty_mm = uncertainties[i];
        assert(seismic_recorder_geometry_solution_consistent(&solution) ==
            (uncertainties[i] >= 0 && uncertainties[i] <= 750));
    }
    solution.node_count = NRF_GEOMETRY_MAX_NODES + 1U;
    assert(!seismic_recorder_geometry_solution_consistent(&solution));

    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    set_assigned(&topology, 0U, 1U);
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));

    memset(&solution, 0, sizeof(solution));
    solution.node_count = 1U;
    solution.nodes[0].node_id = 1U;
    solution.nodes[0].solved = true;
    solution.nodes[0].residual_mm = 42;
    solution.nodes[0].uncertainty_mm = 180;
    object_store_geometry_t geometry;
    seismic_recorder_geometry_apply_solution(&solution, &topology, channels, 1ULL, &geometry);
    assert(geometry.pods[0].residual_mm == 42);
    assert(geometry.pods[0].uncertainty_mm == 180);
}

int main(void)
{
    test_build_inputs_requires_main();
    test_build_inputs_skips_unassigned_and_invalid_sensor_snapshot();
    test_build_inputs_prefers_the_height_the_placement_settled();
    test_build_inputs_rejects_overflow();
    test_apply_solution_maps_pods_and_skips_unsolved();
    test_planar_policy_keeps_all_nodes_at_zero_z();
    test_barometric_relative_policy_subtracts_main_reference();
    test_invalid_references_fall_back_for_whole_recording();
    test_unknown_z_policy_is_rejected();
    test_realistic_altitude_does_not_collapse_layout();
    test_solution_consistency_gate_and_residual_mapping();
    return 0;
}
