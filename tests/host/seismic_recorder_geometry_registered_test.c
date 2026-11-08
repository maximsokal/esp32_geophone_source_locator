#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "seismic_recorder_geometry_policy.h"

/*
 * REMOTE_ONLY localization geometry: the registered AprilTag-centre positions replace the Channel Sounding layout of the
 * session, all or nothing, only for a REMOTE_ONLY session and only for the geometry they were made for. A legacy
 * (MAIN present) session is never touched by them.
 */

#define GEOMETRY_AT_MS 4000U

/* A session recorded in REMOTE_ONLY with `pods` REMOTEs (G1..), as its metadata says. */
static seismic_recorder_session_participation_t remote_only_session(unsigned pods)
{
    seismic_recorder_session_participation_t participation;
    memset(&participation, 0, sizeof(participation));
    participation.geometry_provenance.available = true;
    snprintf(participation.geometry_provenance.topology_mode, sizeof(participation.geometry_provenance.topology_mode),
        "remote_only");
    participation.geometry_provenance.anchor_known = true;
    participation.geometry_provenance.anchor_node_id = 1U;
    for (unsigned i = 0U; i < pods; ++i) {
        participation.pods[i] = (seismic_recorder_pod_participation_t){
            .assigned = true, .node_id_known = true, .node_id = (uint8_t)(i + 1U), .hardware_id_length = 4U,
            .hardware_id = {0xB0U, (uint8_t)i, 0x00U, 0x02U},
            .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING,
        };
    }
    return participation;
}

/* 1 m square on the floor plane, G1..G4, project frame (Z up), tag centres 20 mm above the floor. */
static seismic_recorder_registered_positions_t square_positions(void)
{
    static const int32_t xy[4][2] = {{100, 200}, {1100, 200}, {1100, 1200}, {100, 1200}};
    seismic_recorder_registered_positions_t registered;
    memset(&registered, 0, sizeof(registered));
    registered.available = true;
    registered.geometry_computed_at_ms = GEOMETRY_AT_MS;
    for (unsigned i = 0U; i < 4U; ++i) {
        registered.nodes[i] = (seismic_recorder_registered_position_t){
            .valid = true, .node_id = (uint8_t)(i + 1U), .tag_id = (uint8_t)(i + 1U),
            .x_mm = xy[i][0], .y_mm = xy[i][1], .z_mm = 20,
        };
    }
    return registered;
}

static object_store_geometry_t ble_geometry(unsigned pods)
{
    object_store_geometry_t geometry;
    memset(&geometry, 0, sizeof(geometry));
    for (unsigned i = 0U; i < pods; ++i) {
        geometry.pods[i] = (object_store_pod_geometry_t){
            .computed = true, .leveled = true, .x_mm = (int32_t)(7 * i), .y_mm = (int32_t)(9 * i), .z_mm = 0,
            .residual_mm = 40, .uncertainty_mm = 150, .computed_at_ms = GEOMETRY_AT_MS,
        };
    }
    return geometry;
}

static bool geometry_equal(const object_store_geometry_t *a, const object_store_geometry_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void registered_positions_become_the_geometry(void)
{
    const seismic_recorder_session_participation_t session = remote_only_session(4U);
    const seismic_recorder_registered_positions_t registered = square_positions();
    object_store_geometry_t geometry = ble_geometry(4U);
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_OK);
    assert(geometry.anchored);
    for (unsigned i = 0U; i < 4U; ++i) {
        assert(geometry.pods[i].computed && geometry.pods[i].leveled); /* leveling is the sensor's, kept */
        assert(geometry.pods[i].x_mm == registered.nodes[i].x_mm);     /* exactly the tag centre: offset 0 */
        assert(geometry.pods[i].y_mm == registered.nodes[i].y_mm);
        assert(geometry.pods[i].z_mm == registered.nodes[i].z_mm);
        assert(geometry.pods[i].uncertainty_mm == 0 && geometry.pods[i].residual_mm == 0);
        assert(geometry.pods[i].computed_at_ms == GEOMETRY_AT_MS);     /* the frozen geometry's identity is unchanged */
    }
}

static void participation_says_where_the_positions_came_from(void)
{
    nrf_topology_snapshot_t topology;
    memset(&topology, 0, sizeof(topology));
    topology.main.state = NRF_TOPOLOGY_NODE_ABSENT;
    for (unsigned i = 0U; i < 4U; ++i) {
        topology.remotes[i].state = NRF_TOPOLOGY_NODE_ASSIGNED;
        topology.remotes[i].node_id = (uint8_t)(i + 1U);
    }
    const seismic_recorder_session_participation_t session = remote_only_session(4U);
    const seismic_recorder_registered_positions_t registered = square_positions();
    object_store_geometry_t geometry = ble_geometry(4U);
    seismic_recorder_geometry_z_provenance_t provenance;
    seismic_recorder_session_participation_t built;
    memset(&provenance, 0, sizeof(provenance));
    provenance.available = true;

    assert(seismic_recorder_geometry_build_participation(&topology, &geometry, &provenance, &built) == ESP_OK);
    for (unsigned i = 0U; i < 4U; ++i)
        assert(built.pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);

    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_OK);
    assert(seismic_recorder_geometry_build_participation(&topology, &geometry, &provenance, &built) == ESP_OK);
    for (unsigned i = 0U; i < 4U; ++i)
        assert(built.pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX);
}

static void a_missing_or_foreign_position_changes_nothing(void)
{
    const seismic_recorder_session_participation_t session = remote_only_session(4U);
    const object_store_geometry_t original = ble_geometry(4U);
    object_store_geometry_t geometry = original;
    seismic_recorder_registered_positions_t registered = square_positions();

    registered.nodes[2].valid = false; /* no valid tag 3: no position for G3, never a zero */
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_NOT_FOUND);
    assert(geometry_equal(&geometry, &original));

    registered = square_positions();
    registered.available = false;
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_NOT_FOUND);
    assert(geometry_equal(&geometry, &original));

    registered = square_positions();
    registered.nodes[1].node_id = 3U; /* the position of another node */
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_INVALID_STATE);
    registered = square_positions();
    registered.nodes[3].tag_id = 1U; /* tag of another geophone */
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_INVALID_STATE);
    assert(geometry_equal(&geometry, &original));

    registered = square_positions();
    registered.nodes[0].x_mm = registered.nodes[1].x_mm; /* two geophones on top of each other */
    registered.nodes[0].y_mm = registered.nodes[1].y_mm;
    registered.nodes[0].z_mm = registered.nodes[1].z_mm;
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_INVALID_ARG);
    assert(geometry_equal(&geometry, &original));

    assert(seismic_recorder_geometry_apply_registered_positions(NULL, &registered, &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_apply_registered_positions(&session, NULL, &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, NULL) == ESP_ERR_INVALID_ARG);
}

static void only_a_remote_only_session_takes_registered_positions(void)
{
    const object_store_geometry_t original = ble_geometry(4U);
    object_store_geometry_t geometry = original;
    const seismic_recorder_registered_positions_t registered = square_positions();

    seismic_recorder_session_participation_t legacy = remote_only_session(4U);
    snprintf(legacy.geometry_provenance.topology_mode, sizeof(legacy.geometry_provenance.topology_mode), "main_present");
    assert(seismic_recorder_geometry_apply_registered_positions(&legacy, &registered, &geometry) == ESP_ERR_INVALID_STATE);

    legacy.geometry_provenance.topology_mode[0] = '\0'; /* a session older than the field: read by the old scheme */
    legacy.geometry_provenance.anchor_known = false;
    assert(seismic_recorder_geometry_apply_registered_positions(&legacy, &registered, &geometry) == ESP_ERR_INVALID_STATE);

    seismic_recorder_session_participation_t no_provenance = remote_only_session(4U);
    no_provenance.geometry_provenance.available = false;
    assert(seismic_recorder_geometry_apply_registered_positions(&no_provenance, &registered, &geometry) == ESP_ERR_INVALID_STATE);
    assert(geometry_equal(&geometry, &original)); /* the legacy geometry is exactly what it was */
}

static void positions_for_another_geometry_are_refused(void)
{
    const seismic_recorder_session_participation_t session = remote_only_session(4U);
    const object_store_geometry_t original = ble_geometry(4U);
    object_store_geometry_t geometry = original;
    seismic_recorder_registered_positions_t registered = square_positions();
    registered.geometry_computed_at_ms = GEOMETRY_AT_MS + 1U; /* made for a geometry that was recomputed since */
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_ERR_INVALID_STATE);
    assert(geometry_equal(&geometry, &original));
}

static void a_partial_set_works_only_for_the_computed_pods(void)
{
    /* Two diagnostic REMOTEs: both need their position; the other pods are not asked for one and stay uncomputed. */
    const seismic_recorder_session_participation_t session = remote_only_session(2U);
    const seismic_recorder_registered_positions_t registered = square_positions();
    object_store_geometry_t geometry = ble_geometry(2U);
    assert(seismic_recorder_geometry_apply_registered_positions(&session, &registered, &geometry) == ESP_OK);
    assert(geometry.pods[0].computed && geometry.pods[1].computed);
    assert(!geometry.pods[2].computed && !geometry.pods[3].computed);
    assert(geometry.pods[2].x_mm == 0 && geometry.pods[3].x_mm == 0);

    object_store_geometry_t single = ble_geometry(1U);
    const seismic_recorder_session_participation_t one = remote_only_session(1U);
    assert(seismic_recorder_geometry_apply_registered_positions(&one, &registered, &single) == ESP_ERR_NOT_FOUND);
}

/* No argument runs everything; one argument runs the named case (to show a single RED/GREEN case). */
int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"registered_positions_become_the_geometry", registered_positions_become_the_geometry},
        {"participation_says_where_the_positions_came_from", participation_says_where_the_positions_came_from},
        {"a_missing_or_foreign_position_changes_nothing", a_missing_or_foreign_position_changes_nothing},
        {"only_a_remote_only_session_takes_registered_positions", only_a_remote_only_session_takes_registered_positions},
        {"positions_for_another_geometry_are_refused", positions_for_another_geometry_are_refused},
        {"a_partial_set_works_only_for_the_computed_pods", a_partial_set_works_only_for_the_computed_pods},
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
