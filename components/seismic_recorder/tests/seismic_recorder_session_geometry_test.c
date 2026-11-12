#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

#include "seismic_recorder_internal.h"
#include "storage_manager.h"


static char s_persisted_json[4096];
static bool s_session_exists = true;


esp_err_t seismic_recorder_fs_build_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    const int written = snprintf(
        path, path_size, "%s/%s/sessions/%s",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    return written < 0 || written >= (int)path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    (void)path;
    if (!s_session_exists) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlen(s_persisted_json) + 1U > buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, s_persisted_json, strlen(s_persisted_json) + 1U);
    return ESP_OK;
}


esp_err_t storage_manager_read_text_known_present(const char *path, char *buffer, size_t buffer_size)
{
    return storage_manager_read_text(path, buffer, buffer_size);
}


esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    (void)path;
    if (text == NULL || strlen(text) + 1U > sizeof(s_persisted_json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(s_persisted_json, sizeof(s_persisted_json), "%s", text);
    s_session_exists = true;
    return ESP_OK;
}


const char *seismic_recorder_stop_stage_name(seismic_recorder_stop_stage_t stage)
{
    (void)stage;
    return "test";
}


static void set_session_json(const char *json)
{
    snprintf(s_persisted_json, sizeof(s_persisted_json), "%s", json);
    s_session_exists = true;
}


static void test_valid_geometry_round_trips(void)
{
    set_session_json(
        "{\"schema_version\":1,\"geometry\":{\"anchored\":false,\"pods\":["
        "{\"computed\":true,\"leveled\":true,\"x_mm\":300,\"y_mm\":0,\"z_mm\":10,\"computed_at_ms\":123456},"
        "{\"computed\":true,\"leveled\":false,\"x_mm\":150,\"y_mm\":200,\"z_mm\":5,\"computed_at_ms\":123457},"
        "{\"computed\":false,\"leveled\":false,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"computed_at_ms\":0},"
        "{\"computed\":true,\"leveled\":true,\"x_mm\":-150,\"y_mm\":-200,\"z_mm\":-5,\"computed_at_ms\":123458}"
        "]}}"
    );

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(!geometry.anchored);
    assert(geometry.pods[0].computed && geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 300 && geometry.pods[0].y_mm == 0 && geometry.pods[0].z_mm == 10);
    assert(geometry.pods[0].computed_at_ms == 123456ULL);

    assert(geometry.pods[1].computed && !geometry.pods[1].leveled);
    assert(geometry.pods[1].x_mm == 150 && geometry.pods[1].y_mm == 200);

    assert(!geometry.pods[2].computed);

    assert(geometry.pods[3].computed && geometry.pods[3].leveled);
    assert(geometry.pods[3].x_mm == -150 && geometry.pods[3].y_mm == -200 && geometry.pods[3].z_mm == -5);
}


static void test_missing_geometry_key_defaults_to_zeroed(void)
{
    /* A session recorded before MAX-166 ever existed. */
    set_session_json("{\"schema_version\":1,\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\"}");

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(!geometry.anchored);
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(!geometry.pods[i].computed);
    }
}


static void test_malformed_pod_entry_is_skipped_not_fatal(void)
{
    set_session_json(
        "{\"schema_version\":1,\"geometry\":{\"anchored\":true,\"pods\":["
        "{\"computed\":true,\"leveled\":true,\"x_mm\":\"not-a-number\",\"y_mm\":0,\"z_mm\":0},"
        "{\"computed\":true,\"leveled\":true,\"x_mm\":10,\"y_mm\":20,\"z_mm\":30,\"computed_at_ms\":999}"
        "]}}"
    );

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(geometry.anchored);
    assert(!geometry.pods[0].computed);
    assert(geometry.pods[1].computed);
    assert(geometry.pods[1].x_mm == 10 && geometry.pods[1].y_mm == 20 && geometry.pods[1].z_mm == 30);
}


static void test_missing_file_is_a_real_error(void)
{
    s_session_exists = false;
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_ERR_NOT_FOUND);
    s_session_exists = true;
}


static void test_unparseable_json_is_a_real_error(void)
{
    set_session_json("not-json-at-all");
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_ERR_INVALID_RESPONSE);
}


static void test_rejects_null_arguments(void)
{
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry(NULL, "S000001", &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_geometry("OBJ_0001", NULL, &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", NULL) == ESP_ERR_INVALID_ARG);
}


static void test_v2_writer_and_participation_reader_round_trip(void)
{
    object_store_profile_t profile = {0};
    snprintf(profile.summary.name, sizeof(profile.summary.name), "Participation test");
    profile.geometry.pods[0] = (object_store_pod_geometry_t) {
        .computed = true, .leveled = true, .x_mm = 100, .y_mm = -200, .z_mm = 30,
        .residual_mm = 4, .uncertainty_mm = 5, .computed_at_ms = 1234U,
    };
    profile.geometry.pods[3] = (object_store_pod_geometry_t) {
        .computed = true, .leveled = false, .x_mm = -10, .y_mm = 20, .z_mm = -30,
        .residual_mm = 6, .uncertainty_mm = 7, .computed_at_ms = 5678U,
    };

    seismic_recorder_session_participation_t participation = {0};
    participation.z_provenance.available = true;
    participation.z_provenance.effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE;
    participation.geometry_provenance = (seismic_recorder_geometry_provenance_t) {
        .available = true,
        .solver = "nrf_geometry_solver",
        .solver_version = "1",
        .refined = true,
        .refine_iterations = 4U,
        .topology_generation = 901U,
        .cs_membership_generation = 77U,
        .main_height_reference_generation = 23U,
        .setup_id = "1234-901-77-23",
        .start_uptime_ms = 1200U,
        .ranging_source = "workflow_ranging",
    };
    participation.pods[0] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .node_id_known = true,
        .node_id = 31U,
        .hardware_id_length = 3U,
        .hardware_id = {0x01U, 0xABU, 0xFFU},
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO,
        .source_sample_uptime_ms = 1190U,
        .esp_receipt_uptime_ms = 1195U,
        .source_age_at_start_known = true,
        .source_age_ms_at_start = 5U,
        .height_reference_generation = 23U,
    };
    participation.pods[1] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .node_id_known = true,
        .node_id = 32U,
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_NONE,
    };
    participation.pods[2] = (seismic_recorder_pod_participation_t) {
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_NONE,
    };
    participation.pods[3] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO,
    };
    profile.cs_pairs.pair_count = 1U;
    profile.cs_pairs.pairs[0] = (object_store_cs_pair_t) {
        .node_a = 0U,
        .node_b = 1U,
        .valid = true,
        .distance_mm = 300,
        .confidence = 900U,
        .attempts = 2U,
        .valid_samples = 8U,
        .spread_mm = 11,
        .rejected_samples = 1U,
        .state = "valid",
        .last_failure = "timeout",
        .provider_quality_flags = 5U,
        .scheduler_quality_flags = 1U,
    };

    seismic_recorder_snapshot_t snapshot = {0};
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
    snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
    snapshot.sample_rate_hz = 500U;
    snapshot.channel_count = SEISMIC_CHANNEL_COUNT;

    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) == ESP_OK);
    assert(strstr(s_persisted_json, "\"schema_version\":2") != NULL);
    assert(strstr(s_persisted_json, "\"z_policy_effective\":\"barometric_relative\"") != NULL);
    assert(strstr(s_persisted_json, "\"z_fallback_reason\":\"\"") != NULL);
    assert(strstr(s_persisted_json, "\"position_source\":\"channel_sounding_baro\"") != NULL);
    assert(strstr(s_persisted_json, "\"hardware_id\":\"01abff\"") != NULL);
    assert(strstr(s_persisted_json, "\"node_id\":null") != NULL);
    assert(strstr(s_persisted_json, "\"hardware_id\":null") != NULL);
    assert(strstr(s_persisted_json, "\"solver_version\":\"1\"") != NULL);
    assert(strstr(s_persisted_json, "\"setup_id\":\"1234-901-77-23\"") != NULL);
    assert(strstr(s_persisted_json, "\"ranging_source\":\"workflow_ranging\"") != NULL);
    assert(strstr(s_persisted_json, "\"source_age_ms_at_start\":5") != NULL);
    assert(strstr(s_persisted_json, "\"source_age_ms_at_start\":null") != NULL);
    assert(strstr(s_persisted_json, "\"provider_quality_flags\":5") != NULL);

    seismic_recorder_session_participation_t readback;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.pods[0].assigned && readback.pods[0].node_id_known);
    assert(readback.pods[0].node_id == 31U);
    assert(readback.pods[0].hardware_id_length == 3U);
    assert(memcmp(readback.pods[0].hardware_id, participation.pods[0].hardware_id, 3U) == 0);
    assert(readback.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
    assert(readback.pods[1].assigned && readback.pods[1].node_id_known && readback.pods[1].node_id == 32U);
    assert(readback.pods[1].hardware_id_length == 0U);
    assert(readback.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(!readback.pods[2].assigned && !readback.pods[2].node_id_known);
    assert(readback.pods[2].node_id == 0U && readback.pods[2].hardware_id_length == 0U);
    assert(readback.pods[2].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(readback.pods[3].assigned && !readback.pods[3].node_id_known);
    assert(readback.pods[3].hardware_id_length == 0U);
    assert(readback.pods[3].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING_BARO);
    assert(readback.z_provenance.available);
    assert(readback.z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_BAROMETRIC_RELATIVE);
    assert(readback.z_provenance.fallback_reason[0] == '\0');
    assert(readback.geometry_provenance.available);
    assert(strcmp(readback.geometry_provenance.solver, "nrf_geometry_solver") == 0);
    assert(strcmp(readback.geometry_provenance.solver_version, "1") == 0);
    assert(readback.geometry_provenance.refined && readback.geometry_provenance.refine_iterations == 4U);
    assert(readback.geometry_provenance.topology_generation == 901U);
    assert(readback.geometry_provenance.cs_membership_generation == 77U);
    assert(readback.geometry_provenance.main_height_reference_generation == 23U);
    assert(strcmp(readback.geometry_provenance.setup_id, "1234-901-77-23") == 0);
    assert(readback.geometry_provenance.start_uptime_ms == 1200U);
    assert(strcmp(readback.geometry_provenance.ranging_source, "workflow_ranging") == 0);
    const int32_t offsets[] = {300, -150, 0};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        participation.geometry_provenance.range_offset_manual = true;
        participation.geometry_provenance.range_offset_mm = offsets[i];
        assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) == ESP_OK);
        assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
        assert(readback.geometry_provenance.available && readback.geometry_provenance.range_offset_manual);
        assert(readback.geometry_provenance.range_offset_mm == offsets[i]);
    }
    assert(readback.pods[0].source_sample_uptime_ms == 1190U);
    assert(readback.pods[0].esp_receipt_uptime_ms == 1195U);
    assert(readback.pods[0].source_age_at_start_known);
    assert(readback.pods[0].source_age_ms_at_start == 5U);
    assert(readback.pods[0].height_reference_generation == 23U);
    assert(!readback.pods[1].source_age_at_start_known);

    object_store_cs_pairs_t pairs_readback;
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &pairs_readback) == ESP_OK);
    assert(pairs_readback.pair_count == 1U);
    assert(pairs_readback.pairs[0].state[0] != '\0');
    assert(strcmp(pairs_readback.pairs[0].state, "valid") == 0);
    assert(strcmp(pairs_readback.pairs[0].last_failure, "timeout") == 0);
    assert(pairs_readback.pairs[0].provider_quality_flags == 5U);
    assert(pairs_readback.pairs[0].scheduler_quality_flags == 1U);

    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);
    assert(geometry.pods[0].computed && geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 100 && geometry.pods[0].y_mm == -200 && geometry.pods[0].z_mm == 30);
    assert(geometry.pods[0].residual_mm == 4 && geometry.pods[0].uncertainty_mm == 5);
    assert(geometry.pods[0].computed_at_ms == 1234U);
    assert(!geometry.pods[1].computed && !geometry.pods[2].computed);
    assert(geometry.pods[3].computed && !geometry.pods[3].leveled);
    assert(geometry.pods[3].x_mm == -10 && geometry.pods[3].y_mm == 20 && geometry.pods[3].z_mm == -30);

    /* A planar fallback is explicit v2 evidence, and recovery-style rewrites
     * pass NULL while retaining that evidence from the existing session. */
    participation.z_provenance.effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
    snprintf(participation.z_provenance.fallback_reason,
        sizeof(participation.z_provenance.fallback_reason), "reference_generation_mismatch");
    participation.pods[0].position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
    participation.pods[3].position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "complete", &participation) == ESP_OK);
    assert(strstr(s_persisted_json, "\"z_policy_effective\":\"planar\"") != NULL);
    assert(strstr(s_persisted_json, "\"z_fallback_reason\":\"reference_generation_mismatch\"") != NULL);
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.z_provenance.effective_policy == SEISMIC_RECORDER_GEOMETRY_Z_PLANAR);
    assert(strcmp(readback.z_provenance.fallback_reason, "reference_generation_mismatch") == 0);
    assert(readback.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);

    profile.geometry.pods[0].x_mm = 999;
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "complete", NULL) == ESP_OK);
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.pods[0].assigned && readback.pods[0].node_id == 31U);
    assert(readback.pods[0].hardware_id_length == 3U && readback.pods[0].hardware_id[1] == 0xABU);
    assert(readback.pods[3].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(readback.z_provenance.available);
    assert(strcmp(readback.z_provenance.fallback_reason, "reference_generation_mismatch") == 0);
}


static void test_v1_participation_defaults(void)
{
    set_session_json(
        "{\"schema_version\":1,\"geometry\":{\"pods\":["
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3},"
        "{\"computed\":false,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0},"
        "{\"computed\":true,\"x_mm\":4,\"y_mm\":5,\"z_mm\":6}]}}"
    );

    seismic_recorder_session_participation_t participation;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(participation.pods[0].assigned && !participation.pods[0].node_id_known);
    assert(participation.pods[0].node_id == 0U && participation.pods[0].hardware_id_length == 0U);
    assert(participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(!participation.pods[1].assigned && !participation.pods[1].node_id_known);
    assert(participation.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(participation.pods[2].assigned && !participation.pods[2].node_id_known);
    assert(participation.pods[2].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(!participation.pods[3].assigned && !participation.pods[3].node_id_known);
    assert(!participation.z_provenance.available);
    assert(!participation.geometry_provenance.available);
    assert(participation.pods[0].source_sample_uptime_ms == 0U);
    assert(participation.pods[0].esp_receipt_uptime_ms == 0U);
    assert(!participation.pods[0].source_age_at_start_known);
    assert(participation.pods[0].height_reference_generation == 0U);
}


static void assert_v2_bad_pod_is_skipped(const char *bad_pod)
{
    char json[2048];
    const int written = snprintf(
        json,
        sizeof(json),
        "{\"schema_version\":2,\"geometry\":{\"pods\":[%s,"
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,\"assigned\":true,"
        "\"node_id\":7,\"hardware_id\":\"a0ff\",\"position_source\":\"channel_sounding\"}]}}",
        bad_pod
    );
    assert(written > 0 && written < (int)sizeof(json));
    set_session_json(json);

    seismic_recorder_session_participation_t participation;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(!participation.pods[0].assigned && !participation.pods[0].node_id_known);
    assert(participation.pods[0].node_id == 0U && participation.pods[0].hardware_id_length == 0U);
    assert(participation.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    for (size_t i = 0U; i < SEISMIC_RECORDER_HARDWARE_ID_MAX; ++i) {
        assert(participation.pods[0].hardware_id[i] == 0U);
    }
    assert(participation.pods[1].assigned && participation.pods[1].node_id_known);
    assert(participation.pods[1].node_id == 7U && participation.pods[1].hardware_id_length == 2U);
    assert(participation.pods[1].hardware_id[0] == 0xA0U && participation.pods[1].hardware_id[1] == 0xFFU);
    assert(participation.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(participation.pods[1].source_sample_uptime_ms == 0U);
    assert(participation.pods[1].esp_receipt_uptime_ms == 0U);
    assert(!participation.pods[1].source_age_at_start_known);
    assert(participation.pods[1].height_reference_generation == 0U);
    assert(!participation.z_provenance.available);
    assert(!participation.geometry_provenance.available);
}


static void test_v2_malformed_pod_values_are_isolated(void)
{
    static const char *bad_pods[] = {
        "{\"computed\":false,\"assigned\":true,\"node_id\":-1,\"hardware_id\":null,\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":256,\"hardware_id\":null,\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":1.5,\"hardware_id\":null,\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":\"3\",\"hardware_id\":null,\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":\"abc\",\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":\"zz\",\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":\"001122334455667788\",\"position_source\":\"none\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"apriltag\"}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\",\"source_sample_uptime_ms\":-1}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\",\"height_reference_generation\":65536}",
        "{\"computed\":false,\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\",\"source_age_ms_at_start\":\"stale\"}",
    };
    for (size_t i = 0U; i < sizeof(bad_pods) / sizeof(bad_pods[0]); ++i) {
        assert_v2_bad_pod_is_skipped(bad_pods[i]);
    }
}


static void test_v2_malformed_geometry_provenance_is_skipped(void)
{
    set_session_json(
        "{\"schema_version\":2,\"geometry\":{\"provenance\":{"
        "\"solver\":\"nrf_geometry_solver\",\"solver_version\":\"1\","
        "\"refined\":\"yes\",\"refine_iterations\":1,\"topology_generation\":2,"
        "\"cs_membership_generation\":3,\"main_height_reference_generation\":4,"
        "\"setup_id\":\"5-2-3-4\",\"start_uptime_ms\":5},\"pods\":["
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,\"computed_at_ms\":5,"
        "\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\"}]}}"
    );

    seismic_recorder_session_participation_t participation;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(participation.pods[0].assigned);
    assert(!participation.geometry_provenance.available);

    set_session_json(
        "{\"schema_version\":2,\"geometry\":{\"provenance\":{"
        "\"solver\":\"nrf_geometry_solver\",\"solver_version\":\"1\","
        "\"refined\":true,\"refine_iterations\":1,\"topology_generation\":2,"
        "\"cs_membership_generation\":3,\"main_height_reference_generation\":4,"
        "\"setup_id\":\"bad\",\"start_uptime_ms\":5},\"pods\":["
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,\"computed_at_ms\":5,"
        "\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\"}]}}"
    );
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(participation.pods[0].assigned);
    assert(!participation.geometry_provenance.available);
}


static void test_v2_geometry_provenance_defaults_old_source(void)
{
    set_session_json(
        "{\"schema_version\":2,\"geometry\":{\"provenance\":{"
        "\"solver\":\"nrf_geometry_solver\",\"solver_version\":\"1\","
        "\"refined\":true,\"refine_iterations\":1,\"topology_generation\":2,"
        "\"cs_membership_generation\":3,\"main_height_reference_generation\":4,"
        "\"setup_id\":\"5-2-3-4\",\"start_uptime_ms\":5},\"pods\":["
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,\"computed_at_ms\":5,"
        "\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\"}]}}"
    );

    seismic_recorder_session_participation_t participation;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(participation.geometry_provenance.available);
    assert(strcmp(participation.geometry_provenance.ranging_source, "start_cycle") == 0);
    assert(!participation.geometry_provenance.range_offset_manual && participation.geometry_provenance.range_offset_mm == 0);

    set_session_json(
        "{\"schema_version\":2,\"geometry\":{\"provenance\":{"
        "\"solver\":\"nrf_geometry_solver\",\"solver_version\":\"1\","
        "\"refined\":true,\"refine_iterations\":1,\"topology_generation\":2,"
        "\"cs_membership_generation\":3,\"main_height_reference_generation\":4,"
        "\"setup_id\":\"5-2-3-4\",\"start_uptime_ms\":5,"
        "\"ranging_source\":\"unexpected\"},\"pods\":["
        "{\"computed\":true,\"x_mm\":1,\"y_mm\":2,\"z_mm\":3,\"computed_at_ms\":5,"
        "\"assigned\":true,\"node_id\":null,\"hardware_id\":null,\"position_source\":\"none\"}]}}"
    );
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_OK);
    assert(!participation.geometry_provenance.available);
}

static void test_invalid_range_offset_metadata_is_rejected(void)
{
    const char *bad[] = {
        "\"range_offset_mm\":1",
        "\"range_offset_source\":\"manual\"",
        "\"range_offset_mm\":1,\"range_offset_source\":\"none\"",
        "\"range_offset_mm\":0,\"range_offset_source\":\"other\"",
        "\"range_offset_mm\":0.5,\"range_offset_source\":\"manual\"",
        "\"range_offset_mm\":2147483648,\"range_offset_source\":\"manual\"",
        "\"range_offset_mm\":-2147483649,\"range_offset_source\":\"manual\"",
        "\"range_offset_mm\":1e999,\"range_offset_source\":\"manual\"",
        "\"range_offset_mm\":\"1\",\"range_offset_source\":\"manual\"",
    };
    seismic_recorder_session_participation_t participation;
    char json[512];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        snprintf(json, sizeof(json), "{\"schema_version\":2,\"geometry\":{\"provenance\":{%s}}}", bad[i]);
        set_session_json(json);
        assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_ERR_INVALID_RESPONSE);
        assert(seismic_recorder_json_parse_participation_text(json, &participation) == ESP_ERR_INVALID_RESPONSE);
    }
}


static void test_unsupported_schema_versions_are_rejected(void)
{
    static const char *schemas[] = { "0", "3", "\"2\"" };
    for (size_t i = 0U; i < sizeof(schemas) / sizeof(schemas[0]); ++i) {
        char json[128];
        snprintf(json, sizeof(json), "{\"schema_version\":%s,\"geometry\":{}}", schemas[i]);
        set_session_json(json);
        seismic_recorder_session_participation_t participation;
        object_store_geometry_t geometry;
        assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &participation) == ESP_ERR_INVALID_RESPONSE);
        assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_ERR_INVALID_RESPONSE);
    }
}


static void write_complete_metadata_fixture(void)
{
    object_store_profile_t profile = {0};
    seismic_recorder_snapshot_t snapshot = {0};
    seismic_recorder_session_participation_t participation = {0};
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
    snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
    snapshot.sample_rate_hz = 500U;
    snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
    snapshot.samples_written = 2U;
    snapshot.format_version = SEISMIC_RECORDER_BINARY_VERSION_V1;

    assert(seismic_recorder_json_write_metadata(
        &profile,
        &snapshot,
        "complete",
        &participation
    ) == ESP_OK);
}


static void make_directory(const char *path)
{
    errno = 0;
    if (mkdir(path, 0775) != 0) {
        assert(errno == EEXIST);
    }
}


static void samples_fixture_path(char *path, size_t path_size)
{
    char session_path[SEISMIC_RECORDER_PATH_SIZE];
    assert(seismic_recorder_fs_build_session_path(
        "OBJ_0001",
        "S000001",
        session_path,
        sizeof(session_path)
    ) == ESP_OK);
    const int written = snprintf(path, path_size, "%s/samples.bin", session_path);
    assert(written >= 0 && (size_t)written < path_size);
}


static void write_samples_fixture(uint64_t frames_written, uint64_t extra_frames)
{
    char object_directory[SEISMIC_RECORDER_PATH_SIZE];
    char sessions_directory[SEISMIC_RECORDER_PATH_SIZE];
    char directory[SEISMIC_RECORDER_PATH_SIZE];
    make_directory(STORAGE_MANAGER_MOUNT_POINT);
    make_directory(STORAGE_MANAGER_ROOT_PATH);
    make_directory(STORAGE_MANAGER_OBJECTS_PATH);
    (void)snprintf(object_directory, sizeof(object_directory), "%s/OBJ_0001", STORAGE_MANAGER_OBJECTS_PATH);
    make_directory(object_directory);
    (void)snprintf(sessions_directory, sizeof(sessions_directory), "%s/sessions", object_directory);
    make_directory(sessions_directory);
    (void)snprintf(directory, sizeof(directory), "%s/S000001", sessions_directory);
    make_directory(directory);

    char path[SEISMIC_RECORDER_PATH_SIZE];
    samples_fixture_path(path, sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    seismic_recorder_binary_header_t header = {0};
    memcpy(header.magic, "GEOSEIS1", sizeof(header.magic));
    header.version = SEISMIC_RECORDER_BINARY_VERSION_V1;
    header.channel_count = SEISMIC_CHANNEL_COUNT;
    header.sample_rate_hz = 500U;
    header.frame_size_bytes = sizeof(seismic_frame_t);
    assert(fwrite(&header, 1U, sizeof(header), file) == sizeof(header));
    const seismic_frame_t frame = {0};
    for (uint64_t index = 0U; index < frames_written + extra_frames; ++index) {
        assert(fwrite(&frame, 1U, sizeof(frame), file) == sizeof(frame));
    }
    assert(fclose(file) == 0);
}


static void remove_samples_fixture(void)
{
    char path[SEISMIC_RECORDER_PATH_SIZE];
    samples_fixture_path(path, sizeof(path));
    (void)remove(path);
    (void)snprintf(path, sizeof(path), "%s/OBJ_0001/sessions/S000001", STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/OBJ_0001/sessions", STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/OBJ_0001", STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(path);
    (void)rmdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)rmdir(STORAGE_MANAGER_ROOT_PATH);
    (void)rmdir(STORAGE_MANAGER_MOUNT_POINT);
}


static void replace_schema_version(
    const char *source,
    const char *replacement,
    char *output,
    size_t output_size
)
{
    static const char key[] = "\"schema_version\":";
    const char *schema = strstr(source, key);
    assert(schema != NULL);
    const char *value = schema + sizeof(key) - 1U;
    const char *end = value;
    if (*end == '"') {
        ++end;
        while (*end != '\0' && *end != '"') {
            ++end;
        }
        assert(*end == '"');
        ++end;
    } else {
        while ((*end >= '0' && *end <= '9') || *end == '.' || *end == '-' || *end == '+' || *end == 'e' || *end == 'E') {
            ++end;
        }
    }

    const int written = snprintf(
        output,
        output_size,
        "%.*s%s%s",
        (int)(value - source),
        source,
        replacement,
        end
    );
    assert(written > 0 && (size_t)written < output_size);
}


static void test_summary_reader_accepts_v1_v2_and_rejects_unsupported_versions(void)
{
    write_complete_metadata_fixture();
    write_samples_fixture(2U, 0U);
    char v2_json[sizeof(s_persisted_json)];
    snprintf(v2_json, sizeof(v2_json), "%s", s_persisted_json);

    seismic_recorder_session_summary_t summary;
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(summary.readable && summary.complete && !summary.recovered);
    assert(summary.status == SEISMIC_RECORDER_SESSION_COMPLETE);
    assert(summary.sample_rate_hz == 500U && summary.channel_count == SEISMIC_CHANNEL_COUNT);

    write_samples_fixture(1U, 0U);
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(!summary.complete && !summary.readable && summary.has_gaps);
    assert(summary.status == SEISMIC_RECORDER_SESSION_UNKNOWN);

    remove_samples_fixture();
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(!summary.complete && !summary.readable && summary.has_gaps);

    write_samples_fixture(2U, 1U);
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(summary.complete && summary.readable && !summary.has_gaps);

    char versioned_json[sizeof(s_persisted_json)];
    replace_schema_version(v2_json, "1", versioned_json, sizeof(versioned_json));
    set_session_json(versioned_json);
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(summary.complete && summary.status == SEISMIC_RECORDER_SESSION_COMPLETE);

    static const char *unsupported_versions[] = { "0", "3", "\"2\"" };
    for (size_t i = 0U; i < sizeof(unsupported_versions) / sizeof(unsupported_versions[0]); ++i) {
        replace_schema_version(
            v2_json,
            unsupported_versions[i],
            versioned_json,
            sizeof(versioned_json)
        );
        set_session_json(versioned_json);
        assert(
            seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) ==
            ESP_ERR_INVALID_RESPONSE
        );
    }
    remove_samples_fixture();
}


static void test_summary_gap_classes_round_trip(void)
{
    static const struct {
        uint64_t ring_dropped_frames;
        uint64_t source_gap_samples;
        bool has_gaps;
    } cases[] = {
        {0U, 0U, false},
        {3U, 0U, true},
        {0U, 2U, true},
        {3U, 2U, true},
    };
    write_samples_fixture(2U, 0U);

    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        object_store_profile_t profile = {0};
        seismic_recorder_snapshot_t snapshot = {0};
        seismic_recorder_session_participation_t participation = {0};
        snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
        snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
        snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
        snapshot.sample_rate_hz = 500U;
        snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
        snapshot.samples_written = 2U;
        snapshot.dropped_frames = cases[index].ring_dropped_frames;
        snapshot.source_gap_samples = cases[index].source_gap_samples;
        snapshot.format_version = SEISMIC_RECORDER_BINARY_VERSION_V1;

        assert(seismic_recorder_json_write_metadata(
            &profile, &snapshot, "complete", &participation
        ) == ESP_OK);
        assert(strstr(s_persisted_json, cases[index].has_gaps
            ? "\"has_gaps\":true" : "\"has_gaps\":false") != NULL);
        char counter[64];
        snprintf(counter, sizeof(counter), "\"ring_dropped_frames\":%llu",
            (unsigned long long)cases[index].ring_dropped_frames);
        assert(strstr(s_persisted_json, counter) != NULL);
        snprintf(counter, sizeof(counter), "\"source_gap_samples\":%llu",
            (unsigned long long)cases[index].source_gap_samples);
        assert(strstr(s_persisted_json, counter) != NULL);

        seismic_recorder_session_summary_t summary;
        assert(seismic_recorder_json_read_summary(
            "OBJ_0001", "S000001", &summary
        ) == ESP_OK);
        assert(summary.complete && summary.readable);
        assert(summary.ring_dropped_frames == cases[index].ring_dropped_frames);
        assert(summary.source_gap_samples == cases[index].source_gap_samples);
        assert(summary.has_gaps == cases[index].has_gaps);
        assert(summary.missing_samples ==
            cases[index].ring_dropped_frames + cases[index].source_gap_samples);
    }

    remove_samples_fixture();
}


static void test_recording_gate_metadata_round_trips(void)
{
    object_store_profile_t profile = {0};
    seismic_recorder_snapshot_t snapshot = {0};
    seismic_recorder_session_participation_t participation = {0};
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
    snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
    snapshot.sample_rate_hz = 500U;
    snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
    participation.recording_gate = (seismic_recorder_gate_metadata_t){
        .available = true,
        .gate_bypassed = true,
        .assigned_remote_count = 2U,
        .accepted_remote_count = 1U,
        .leveling_state = SEISMIC_RECORDER_LEVELING_INCOMPLETE,
    };

    assert(seismic_recorder_json_write_metadata(
        &profile, &snapshot, "recording", &participation
    ) == ESP_OK);
    assert(strstr(s_persisted_json, "\"gate_bypassed\":true") != NULL);
    assert(strstr(s_persisted_json, "\"assigned_remote_count\":2") != NULL);
    assert(strstr(s_persisted_json, "\"accepted_remote_count\":1") != NULL);
    assert(strstr(s_persisted_json, "\"leveling_state\":\"incomplete\"") != NULL);

    seismic_recorder_session_participation_t readback = {0};
    assert(seismic_recorder_read_session_participation(
        "OBJ_0001", "S000001", &readback
    ) == ESP_OK);
    assert(readback.recording_gate.available);
    assert(readback.recording_gate.gate_bypassed);
    assert(readback.recording_gate.assigned_remote_count == 2U);
    assert(readback.recording_gate.accepted_remote_count == 1U);
    assert(readback.recording_gate.leveling_state == SEISMIC_RECORDER_LEVELING_INCOMPLETE);
}


static seismic_recorder_session_participation_t remote_only_participation(void)
{
    seismic_recorder_session_participation_t participation = {0};
    participation.z_provenance.available = true;
    participation.z_provenance.effective_policy = SEISMIC_RECORDER_GEOMETRY_Z_PLANAR;
    snprintf(participation.z_provenance.fallback_reason, sizeof(participation.z_provenance.fallback_reason),
        "main_reference_invalid");
    participation.geometry_provenance = (seismic_recorder_geometry_provenance_t) {
        .available = true, .solver = "nrf_geometry_solver", .solver_version = "1", .refined = true,
        .refine_iterations = 3U, .topology_generation = 12U, .cs_membership_generation = 5U,
        .main_height_reference_generation = 0U, .setup_id = "4000-12-5-0", .start_uptime_ms = 4100U,
        .ranging_source = "workflow_ranging", .topology_mode = "remote_only", .anchor_known = true, .anchor_node_id = 1U,
    };
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        participation.pods[i] = (seismic_recorder_pod_participation_t) {
            .assigned = true, .node_id_known = true, .node_id = (uint8_t)(i + 1U),
            .hardware_id_length = 4U, .hardware_id = {0xB0U, (uint8_t)i, 0x00U, 0x02U},
            .position_source = SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX,
            .tag_id_known = true, .tag_id = (uint8_t)(i + 1U),
        };
    }
    return participation;
}

static object_store_profile_t remote_only_profile(void)
{
    object_store_profile_t profile = {0};
    snprintf(profile.summary.name, sizeof(profile.summary.name), "REMOTE_ONLY test");
    profile.geometry.anchored = true;
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        profile.geometry.pods[i] = (object_store_pod_geometry_t) {
            .computed = true, .leveled = true, .x_mm = (int32_t)(100 + 1000 * (i % 2U)), .y_mm = (int32_t)(200 + 1000 * (i / 2U)),
            .z_mm = 20, .computed_at_ms = 4000U,
        };
    }
    return profile;
}

static seismic_recorder_snapshot_t remote_only_snapshot(void)
{
    seismic_recorder_snapshot_t snapshot = {0};
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
    snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
    snapshot.sample_rate_hz = 500U;
    snapshot.channel_count = SEISMIC_CHANNEL_COUNT;
    return snapshot;
}

static void test_remote_only_session_round_trip(void)
{
    object_store_profile_t profile = remote_only_profile();
    seismic_recorder_session_participation_t participation = remote_only_participation();
    seismic_recorder_snapshot_t snapshot = remote_only_snapshot();
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) == ESP_OK);
    assert(strstr(s_persisted_json, "\"position_source\":\"apriltag_center_approx\"") != NULL);
    assert(strstr(s_persisted_json, "\"tag_id\":4") != NULL);
    assert(strstr(s_persisted_json, "\"topology_mode\":\"remote_only\"") != NULL);
    assert(strstr(s_persisted_json, "\"anchor_node_id\":1") != NULL);
    assert(strstr(s_persisted_json, "\"hardware_id\":\"b0010002\"") != NULL);

    seismic_recorder_session_participation_t readback;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(readback.pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_APRILTAG_CENTER_APPROX);
        assert(readback.pods[i].tag_id_known && readback.pods[i].tag_id == i + 1U);
        assert(readback.pods[i].node_id_known && readback.pods[i].node_id == i + 1U);
        assert(readback.pods[i].hardware_id_length == 4U && readback.pods[i].hardware_id[1] == i);
    }
    assert(readback.geometry_provenance.available);
    assert(strcmp(readback.geometry_provenance.topology_mode, "remote_only") == 0);
    assert(readback.geometry_provenance.anchor_known && readback.geometry_provenance.anchor_node_id == 1U);
}

static void test_session_without_topology_fields_reads_as_legacy(void)
{
    object_store_profile_t profile = remote_only_profile();
    profile.geometry.anchored = false;
    seismic_recorder_session_participation_t participation = remote_only_participation();
    participation.geometry_provenance.topology_mode[0] = '\0';
    participation.geometry_provenance.anchor_known = false;
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        participation.pods[i].position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING;
        participation.pods[i].tag_id_known = false;
    }
    seismic_recorder_snapshot_t snapshot = remote_only_snapshot();
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) == ESP_OK);
    /* A session of the old kind is written exactly as before: the new keys are not there to be misread. */
    assert(strstr(s_persisted_json, "topology_mode") == NULL && strstr(s_persisted_json, "anchor_node_id") == NULL);
    assert(strstr(s_persisted_json, "tag_id") == NULL);
    seismic_recorder_session_participation_t readback;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.geometry_provenance.available);
    assert(readback.geometry_provenance.topology_mode[0] == '\0' && !readback.geometry_provenance.anchor_known);
    for (unsigned i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(readback.pods[i].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
        assert(!readback.pods[i].tag_id_known);
    }
}

static void test_unknown_topology_mode_is_refused_not_guessed(void)
{
    object_store_profile_t profile = remote_only_profile();
    seismic_recorder_session_participation_t participation = remote_only_participation();
    snprintf(participation.geometry_provenance.topology_mode, sizeof(participation.geometry_provenance.topology_mode),
        "main_absent");
    seismic_recorder_snapshot_t snapshot = remote_only_snapshot();
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) != ESP_OK);
    /* An anchor without a mode (or the reverse) is not a coherent record either. */
    participation = remote_only_participation();
    participation.geometry_provenance.topology_mode[0] = '\0';
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) != ESP_OK);
}

int main(void)
{
    test_valid_geometry_round_trips();
    test_summary_gap_classes_round_trip();
    test_missing_geometry_key_defaults_to_zeroed();
    test_malformed_pod_entry_is_skipped_not_fatal();
    test_missing_file_is_a_real_error();
    test_unparseable_json_is_a_real_error();
    test_rejects_null_arguments();
    test_v2_writer_and_participation_reader_round_trip();
    test_v1_participation_defaults();
    test_v2_malformed_pod_values_are_isolated();
    test_v2_malformed_geometry_provenance_is_skipped();
    test_v2_geometry_provenance_defaults_old_source();
    test_invalid_range_offset_metadata_is_rejected();
    test_unsupported_schema_versions_are_rejected();
    test_summary_reader_accepts_v1_v2_and_rejects_unsupported_versions();
    test_recording_gate_metadata_round_trips();
    test_remote_only_session_round_trip();
    test_session_without_topology_fields_reads_as_legacy();
    test_unknown_topology_mode_is_refused_not_guessed();
    puts("seismic recorder session geometry tests passed");
    return 0;
}
