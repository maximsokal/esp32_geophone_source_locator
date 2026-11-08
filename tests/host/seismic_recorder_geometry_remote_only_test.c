#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "seismic_recorder_geometry_policy.h"

/*
 * REMOTE_ONLY at recording START: geometry inputs without MAIN and the cable admission of a frozen ranging. The legacy
 * fixtures stay in seismic_recorder_geometry_policy_test.c, untouched.
 */

static void set_assigned(nrf_topology_snapshot_t *topology, uint8_t channel)
{
    topology->remotes[channel].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology->remotes[channel].node_id = (uint8_t)(channel + 1U);
    topology->remotes[channel].mux_channel = channel;
}

static nrf_topology_snapshot_t remote_only_topology(unsigned remotes)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.tca_available = true;
    topology.main.state = NRF_TOPOLOGY_NODE_ABSENT;
    for (unsigned i = 0U; i < remotes; ++i) set_assigned(&topology, (uint8_t)i);
    return topology;
}

static nrf_topology_snapshot_t main_present_topology(unsigned remotes)
{
    nrf_topology_snapshot_t topology = remote_only_topology(remotes);
    topology.main.state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.main.node_id = 0U;
    return topology;
}

static esp_err_t build(const nrf_topology_snapshot_t *topology, const seismic_recorder_geometry_channel_status_t *channels,
                       seismic_recorder_geometry_z_policy_t policy, nrf_geometry_node_input_t *inputs, size_t *count)
{
    const seismic_recorder_geometry_main_reference_t reference = {.valid = true, .generation = 7U, .relative_z_mm = 50};
    seismic_recorder_geometry_z_provenance_t provenance;
    return seismic_recorder_geometry_build_inputs(
        topology, channels, &reference, policy, inputs, NRF_GEOMETRY_MAX_NODES, count, &provenance);
}

static void channels_with_readings(seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT],
                                   const int32_t readings[4])
{
    memset(channels, 0, NRF_TOPOLOGY_REMOTE_COUNT * sizeof(channels[0]));
    for (unsigned i = 0U; i < 4U; ++i) {
        channels[i].sensor_valid = true;
        channels[i].height_reference_valid = true;
        channels[i].height_reference_generation = 7U;
        channels[i].relative_z_mm = readings[i];
    }
}

static void build_inputs_remote_only_has_no_node0(void)
{
    const int32_t readings[4] = {250, -100, 40, 600};
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    const nrf_topology_snapshot_t topology = remote_only_topology(4U);
    channels_with_readings(channels, readings);

    assert(build(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE, inputs, &count) == ESP_OK);
    assert(count == 4U);
    for (unsigned i = 0U; i < 4U; ++i) {
        assert(inputs[i].node_id == i + 1U);                    /* G1..G4, no node 0 */
        assert(inputs[i].z_mm == readings[i] - 50);             /* the anchor G1 too: reading minus the housing reference */
    }
    assert(build(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, inputs, &count) == ESP_OK);
    assert(count == 4U);
    for (unsigned i = 0U; i < 4U; ++i) assert(inputs[i].z_mm == 0);
}

static void build_inputs_requires_a_settled_main(void)
{
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    const int32_t readings[4] = {0, 0, 0, 0};
    channels_with_readings(channels, readings);

    const nrf_topology_node_state_t unsettled[] = {
        NRF_TOPOLOGY_NODE_UNKNOWN, NRF_TOPOLOGY_NODE_CONFLICT, NRF_TOPOLOGY_NODE_TRANSPORT_ERROR,
        NRF_TOPOLOGY_NODE_PROTOCOL_ERROR,
    };
    for (size_t i = 0U; i < sizeof(unsettled) / sizeof(unsettled[0]); ++i) {
        nrf_topology_snapshot_t topology = remote_only_topology(4U);
        topology.main.state = unsettled[i];
        assert(build(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, inputs, &count) == ESP_ERR_INVALID_STATE);
    }
    /* MAIN absent but nobody else there: nothing to build a mesh from. */
    const nrf_topology_snapshot_t empty = remote_only_topology(0U);
    assert(build(&empty, channels, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, inputs, &count) == ESP_ERR_INVALID_STATE);
}

static void build_inputs_main_present_is_unchanged(void)
{
    const int32_t readings[4] = {100, 100, 100, 100};
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    nrf_geometry_node_input_t inputs[NRF_GEOMETRY_MAX_NODES];
    size_t count = 0U;
    const nrf_topology_snapshot_t topology = main_present_topology(4U);
    channels_with_readings(channels, readings);
    assert(build(&topology, channels, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE, inputs, &count) == ESP_OK);
    assert(count == 5U);
    assert(inputs[0].node_id == 0U && inputs[0].z_mm == 0); /* MAIN first, at the reference */
    for (unsigned i = 1U; i < 5U; ++i) assert(inputs[i].node_id == i && inputs[i].z_mm == 50);
}

static const uint32_t k_800[NRF_TOPOLOGY_REMOTE_COUNT] = {800U, 800U, 800U, 800U};

static nrf_cs_pair_result_t valid_pair(nrf_cs_node_id_t a, nrf_cs_node_id_t b, int32_t distance_mm, uint16_t quality_flags)
{
    nrf_cs_pair_result_t pair;
    memset(&pair, 0, sizeof(pair));
    pair.node_a = a;
    pair.node_b = b;
    pair.state = NRF_CS_PAIR_VALID;
    pair.distance_mm = distance_mm;
    pair.scheduler_quality_flags = quality_flags;
    return pair;
}

static void remote_only_cable_admission_bounds_a_pair_by_the_sum_of_its_cables(void)
{
    const nrf_topology_snapshot_t topology = remote_only_topology(4U);
    const uint32_t allowance = 2U * NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM;
    nrf_cs_pair_result_t pair = valid_pair(1U, 2U, 1600 + (int32_t)allowance, 0U);

    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &pair, 1U, k_800) == ESP_OK); /* automatic: bin allowance */
    pair.distance_mm += 1;
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &pair, 1U, k_800) == ESP_ERR_INVALID_STATE);

    pair = valid_pair(1U, 2U, 1600, NRF_CS_QUALITY_MANUAL); /* manual: exact */
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &pair, 1U, k_800) == ESP_OK);
    pair.distance_mm = 1601;
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &pair, 1U, k_800) == ESP_ERR_INVALID_STATE);

    /* A pair that was not measured says nothing about the cables. */
    pair = valid_pair(1U, 2U, 99999, 0U);
    pair.state = NRF_CS_PAIR_MISSING;
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &pair, 1U, k_800) == ESP_OK);

    /* Every assigned REMOTE needs a cable. */
    uint32_t partial[NRF_TOPOLOGY_REMOTE_COUNT] = {800U, 800U, 0U, 800U};
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, NULL, 0U, partial) == ESP_ERR_INVALID_ARG);
}

static void main_present_cable_admission_keeps_the_main_edge_rule(void)
{
    const nrf_topology_snapshot_t topology = main_present_topology(2U);
    nrf_cs_pair_result_t edge = valid_pair(0U, 1U, 800 + (int32_t)NRF_TOPOLOGY_CABLE_MEASUREMENT_SLACK_MM, 0U);
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &edge, 1U, k_800) == ESP_OK);
    edge.distance_mm += 1;
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &edge, 1U, k_800) == ESP_ERR_INVALID_STATE);
    /* Pairs between two REMOTEs are not bounded by the sum rule while MAIN is present (the legacy behaviour). */
    nrf_cs_pair_result_t between = valid_pair(1U, 2U, 5000, 0U);
    assert(seismic_recorder_geometry_check_cable_pairs(&topology, &between, 1U, k_800) == ESP_OK);
}

static object_store_geometry_t geometry_with_node_at(unsigned pod, int32_t x_mm)
{
    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    geometry.pods[pod].computed = true;
    geometry.pods[pod].x_mm = x_mm;
    return geometry;
}

static void cable_radius_applies_only_with_main(void)
{
    nrf_cs_node_id_t bad = 0xEEU;
    const nrf_topology_snapshot_t with_main = main_present_topology(2U);
    const nrf_topology_snapshot_t without_main = remote_only_topology(2U);
    const object_store_geometry_t near = geometry_with_node_at(0U, 700);
    const object_store_geometry_t far = geometry_with_node_at(0U, 2500);

    assert(seismic_recorder_geometry_check_cable_radius(&with_main, &near, NULL, 0U, k_800, &bad) == ESP_OK);
    assert(seismic_recorder_geometry_check_cable_radius(&with_main, &far, NULL, 0U, k_800, &bad) == ESP_ERR_INVALID_STATE);
    assert(bad == 1U);
    /* The origin of a REMOTE_ONLY geometry is a REMOTE: its distance from the origin is no cable. */
    bad = 0xEEU;
    assert(seismic_recorder_geometry_check_cable_radius(&without_main, &far, NULL, 0U, k_800, &bad) == ESP_OK);
    assert(bad == 0xEEU);
}

/*
 * What the firmware assumes about the geophones (WIRING_MAPPING_UNVERIFIED: the repository does not record which AD7606
 * channel each geophone is wired to, so this pins the code's own convention and nothing about the copper): G<k> is the
 * REMOTE on mux channel k-1, its CS node id is k, and its pod - the index the localization uses as the AD7606 channel - is
 * k-1. A REMOTE alone on mux channel 2 is node 3 and fills pod 2, never pod 0.
 */
static void pod_index_is_the_mux_channel_and_node_is_channel_plus_one(void)
{
    nrf_topology_snapshot_t topology = remote_only_topology(0U);
    set_assigned(&topology, 2U); /* G3 only */
    nrf_geometry_solution_t solution;
    memset(&solution, 0, sizeof(solution));
    solution.node_count = 1U;
    solution.nodes[0] = (nrf_geometry_node_position_t){.node_id = 3U, .solved = true, .x_mm = 1500, .y_mm = -700, .z_mm = 40};
    seismic_recorder_geometry_channel_status_t channels[NRF_TOPOLOGY_REMOTE_COUNT];
    memset(channels, 0, sizeof(channels));
    object_store_geometry_t geometry;
    seismic_recorder_geometry_apply_solution(&solution, &topology, channels, 777U, &geometry);
    for (unsigned pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) assert(geometry.pods[pod].computed == (pod == 2U));
    assert(geometry.pods[2].x_mm == 1500 && geometry.pods[2].y_mm == -700);

    seismic_recorder_geometry_z_provenance_t provenance;
    seismic_recorder_session_participation_t participation;
    memset(&provenance, 0, sizeof(provenance));
    assert(seismic_recorder_geometry_build_participation(&topology, &geometry, &provenance, &participation) == ESP_OK);
    for (unsigned pod = 0U; pod < OBJECT_STORE_POD_COUNT; ++pod) assert(participation.pods[pod].assigned == (pod == 2U));
    assert(participation.pods[2].node_id_known && participation.pods[2].node_id == 3U);
}

/* No argument runs everything; one argument runs the named case (to show a single RED/GREEN case). */
int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"build_inputs_remote_only_has_no_node0", build_inputs_remote_only_has_no_node0},
        {"build_inputs_requires_a_settled_main", build_inputs_requires_a_settled_main},
        {"build_inputs_main_present_is_unchanged", build_inputs_main_present_is_unchanged},
        {"remote_only_cable_admission_bounds_a_pair_by_the_sum_of_its_cables",
         remote_only_cable_admission_bounds_a_pair_by_the_sum_of_its_cables},
        {"main_present_cable_admission_keeps_the_main_edge_rule", main_present_cable_admission_keeps_the_main_edge_rule},
        {"cable_radius_applies_only_with_main", cable_radius_applies_only_with_main},
        {"pod_index_is_the_mux_channel_and_node_is_channel_plus_one", pod_index_is_the_mux_channel_and_node_is_channel_plus_one},
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
