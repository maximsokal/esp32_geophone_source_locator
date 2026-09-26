#include <assert.h>
#include <string.h>

#include "seismic_recorder_geometry_policy.h"


static esp_err_t build_participation(
    const nrf_topology_snapshot_t *topology,
    const object_store_geometry_t *geometry,
    seismic_recorder_geometry_z_policy_t policy,
    seismic_recorder_session_participation_t *participation
)
{
    const seismic_recorder_geometry_z_provenance_t provenance = {
        .available = true,
        .effective_policy = policy,
    };
    return seismic_recorder_geometry_build_participation(
        topology, geometry, &provenance, participation
    );
}


static void assert_empty_participation(const seismic_recorder_session_participation_t *participation)
{
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(!participation->pods[i].assigned);
        assert(!participation->pods[i].node_id_known);
        assert(participation->pods[i].node_id == 0U);
        assert(participation->pods[i].hardware_id_length == 0U);
        assert(participation->pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
        for (size_t byte = 0U; byte < NRF_UPSTREAM_PROTOCOL_HW_ID_MAX; ++byte) {
            assert(participation->pods[i].hardware_id[byte] == 0U);
        }
    }
}


static void test_no_remotes_and_all_assigned(void)
{
    nrf_topology_snapshot_t topology = {0};
    object_store_geometry_t geometry = {0};
    seismic_recorder_session_participation_t participation;

    assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, &participation) == ESP_OK);
    assert_empty_participation(&participation);

    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        topology.remotes[i].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        topology.remotes[i].node_id = (uint8_t)(10U + i);
        topology.remotes[i].hardware_id_length = (uint8_t)(i + 1U);
        for (size_t byte = 0U; byte < topology.remotes[i].hardware_id_length; ++byte) {
            topology.remotes[i].hardware_id[byte] = (uint8_t)(i * 16U + byte);
        }
        geometry.pods[i].computed = true;
    }

    assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, &participation) == ESP_OK);
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(participation.pods[i].assigned && participation.pods[i].node_id_known);
        assert(participation.pods[i].node_id == 10U + i);
        assert(participation.pods[i].hardware_id_length == i + 1U);
        assert(participation.pods[i].hardware_id[0] == i * 16U);
        assert(participation.pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    }

    assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE, &participation) == ESP_OK);
    assert(participation.z_provenance.available);
    assert(participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
}


static void test_gaps_unassigned_states_and_uncomputed_positions(void)
{
    nrf_topology_snapshot_t topology = {0};
    object_store_geometry_t geometry = {0};
    seismic_recorder_session_participation_t participation;
    topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.remotes[0].node_id = 21U;
    topology.remotes[0].hardware_id_length = 1U;
    topology.remotes[0].hardware_id[0] = 0xA1U;
    topology.remotes[1].state = NRF_TOPOLOGY_NODE_ABSENT;
    topology.remotes[1].node_id = 22U;
    topology.remotes[1].hardware_id_length = 1U;
    topology.remotes[1].hardware_id[0] = 0xA2U;
    topology.remotes[2].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.remotes[2].node_id = 23U;
    topology.remotes[2].hardware_id_length = 1U;
    topology.remotes[2].hardware_id[0] = 0xA3U;
    topology.remotes[3].state = NRF_TOPOLOGY_NODE_CONFLICT;
    topology.remotes[3].node_id = 24U;
    topology.remotes[3].hardware_id_length = 1U;
    topology.remotes[3].hardware_id[0] = 0xA4U;
    geometry.pods[0].computed = true;

    assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, &participation) == ESP_OK);
    assert(participation.pods[0].assigned && participation.pods[0].node_id_known);
    assert(participation.pods[0].node_id == 21U && participation.pods[0].hardware_id[0] == 0xA1U);
    assert(participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(!participation.pods[1].assigned && !participation.pods[1].node_id_known);
    assert(participation.pods[1].node_id == 0U && participation.pods[1].hardware_id_length == 0U);
    assert(participation.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(participation.pods[2].assigned && participation.pods[2].node_id_known);
    assert(participation.pods[2].node_id == 23U && participation.pods[2].hardware_id[0] == 0xA3U);
    assert(participation.pods[2].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(!participation.pods[3].assigned && !participation.pods[3].node_id_known);
    assert(participation.pods[3].node_id == 0U && participation.pods[3].hardware_id_length == 0U);

    const nrf_topology_node_state_t other_unassigned_states[] = {
        NRF_TOPOLOGY_NODE_PROTOCOL_ERROR,
        NRF_TOPOLOGY_NODE_TRANSPORT_ERROR,
        NRF_TOPOLOGY_NODE_UNKNOWN,
    };
    for (size_t i = 0U; i < sizeof(other_unassigned_states) / sizeof(other_unassigned_states[0]); ++i) {
        memset(&topology, 0, sizeof(topology));
        topology.remotes[0].state = other_unassigned_states[i];
        topology.remotes[0].node_id = 90U;
        topology.remotes[0].hardware_id_length = 1U;
        topology.remotes[0].hardware_id[0] = 0x90U;
        memset(&geometry, 0, sizeof(geometry));
        assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, &participation) == ESP_OK);
        assert(!participation.pods[0].assigned && !participation.pods[0].node_id_known);
        assert(participation.pods[0].node_id == 0U && participation.pods[0].hardware_id_length == 0U);
        assert(participation.pods[0].hardware_id[0] == 0U);
    }
}


static void test_hardware_id_length_is_clamped(void)
{
    nrf_topology_snapshot_t topology = {0};
    object_store_geometry_t geometry = {0};
    seismic_recorder_session_participation_t participation;
    topology.remotes[0].state = NRF_TOPOLOGY_NODE_ASSIGNED;
    topology.remotes[0].node_id = 1U;
    topology.remotes[0].hardware_id_length = UINT8_MAX;
    for (size_t i = 0U; i < NRF_UPSTREAM_PROTOCOL_HW_ID_MAX; ++i) {
        topology.remotes[0].hardware_id[i] = (uint8_t)(0xD0U + i);
    }

    assert(build_participation(&topology, &geometry, SEISMIC_RECORDER_GEOMETRY_Z_PLANAR, &participation) == ESP_OK);
    assert(participation.pods[0].hardware_id_length == NRF_UPSTREAM_PROTOCOL_HW_ID_MAX);
    assert(memcmp(
        participation.pods[0].hardware_id,
        topology.remotes[0].hardware_id,
        NRF_UPSTREAM_PROTOCOL_HW_ID_MAX
    ) == 0);
}


static void test_null_arguments_are_rejected(void)
{
    nrf_topology_snapshot_t topology = {0};
    object_store_geometry_t geometry = {0};
    seismic_recorder_session_participation_t participation;
    const seismic_recorder_geometry_z_provenance_t provenance = {0};
    assert(seismic_recorder_geometry_build_participation(NULL, &geometry, &provenance, &participation) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_build_participation(&topology, NULL, &provenance, &participation) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_build_participation(&topology, &geometry, NULL, &participation) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_build_participation(&topology, &geometry, &provenance, NULL) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_no_remotes_and_all_assigned();
    test_gaps_unassigned_states_and_uncomputed_positions();
    test_hardware_id_length_is_clamped();
    test_null_arguments_are_rejected();
    return 0;
}
