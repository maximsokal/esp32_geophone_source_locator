#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    participation.pods[0] = (seismic_recorder_pod_participation_t) {
        .assigned = true,
        .node_id_known = true,
        .node_id = 31U,
        .hardware_id_length = 3U,
        .hardware_id = {0x01U, 0xABU, 0xFFU},
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING,
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
        .position_source = SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING,
    };

    seismic_recorder_snapshot_t snapshot = {0};
    snprintf(snapshot.object_id, sizeof(snapshot.object_id), "OBJ_0001");
    snprintf(snapshot.session_id, sizeof(snapshot.session_id), "S000001");
    snprintf(snapshot.source_name, sizeof(snapshot.source_name), "AD7606");
    snapshot.sample_rate_hz = 500U;
    snapshot.channel_count = SEISMIC_CHANNEL_COUNT;

    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "recording", &participation) == ESP_OK);
    assert(strstr(s_persisted_json, "\"schema_version\":2") != NULL);
    assert(strstr(s_persisted_json, "\"hardware_id\":\"01abff\"") != NULL);
    assert(strstr(s_persisted_json, "\"node_id\":null") != NULL);
    assert(strstr(s_persisted_json, "\"hardware_id\":null") != NULL);

    seismic_recorder_session_participation_t readback;
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.pods[0].assigned && readback.pods[0].node_id_known);
    assert(readback.pods[0].node_id == 31U);
    assert(readback.pods[0].hardware_id_length == 3U);
    assert(memcmp(readback.pods[0].hardware_id, participation.pods[0].hardware_id, 3U) == 0);
    assert(readback.pods[0].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
    assert(readback.pods[1].assigned && readback.pods[1].node_id_known && readback.pods[1].node_id == 32U);
    assert(readback.pods[1].hardware_id_length == 0U);
    assert(readback.pods[1].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(!readback.pods[2].assigned && !readback.pods[2].node_id_known);
    assert(readback.pods[2].node_id == 0U && readback.pods[2].hardware_id_length == 0U);
    assert(readback.pods[2].position_source == SEISMIC_RECORDER_POSITION_SOURCE_NONE);
    assert(readback.pods[3].assigned && !readback.pods[3].node_id_known);
    assert(readback.pods[3].hardware_id_length == 0U);
    assert(readback.pods[3].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);

    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);
    assert(geometry.pods[0].computed && geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 100 && geometry.pods[0].y_mm == -200 && geometry.pods[0].z_mm == 30);
    assert(geometry.pods[0].residual_mm == 4 && geometry.pods[0].uncertainty_mm == 5);
    assert(geometry.pods[0].computed_at_ms == 1234U);
    assert(!geometry.pods[1].computed && !geometry.pods[2].computed);
    assert(geometry.pods[3].computed && !geometry.pods[3].leveled);
    assert(geometry.pods[3].x_mm == -10 && geometry.pods[3].y_mm == 20 && geometry.pods[3].z_mm == -30);

    /* Recovery-style rewrites pass NULL; the real writer must retain the v2
     * participation read from the existing session file. */
    profile.geometry.pods[0].x_mm = 999;
    assert(seismic_recorder_json_write_metadata(&profile, &snapshot, "complete", NULL) == ESP_OK);
    assert(seismic_recorder_read_session_participation("OBJ_0001", "S000001", &readback) == ESP_OK);
    assert(readback.pods[0].assigned && readback.pods[0].node_id == 31U);
    assert(readback.pods[0].hardware_id_length == 3U && readback.pods[0].hardware_id[1] == 0xABU);
    assert(readback.pods[3].position_source == SEISMIC_RECORDER_POSITION_SOURCE_CHANNEL_SOUNDING);
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
    };
    for (size_t i = 0U; i < sizeof(bad_pods) / sizeof(bad_pods[0]); ++i) {
        assert_v2_bad_pod_is_skipped(bad_pods[i]);
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

    assert(seismic_recorder_json_write_metadata(
        &profile,
        &snapshot,
        "complete",
        &participation
    ) == ESP_OK);
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
    char v2_json[sizeof(s_persisted_json)];
    snprintf(v2_json, sizeof(v2_json), "%s", s_persisted_json);

    seismic_recorder_session_summary_t summary;
    assert(seismic_recorder_json_read_summary("OBJ_0001", "S000001", &summary) == ESP_OK);
    assert(summary.readable && summary.complete && !summary.recovered);
    assert(summary.status == SEISMIC_RECORDER_SESSION_COMPLETE);
    assert(summary.sample_rate_hz == 500U && summary.channel_count == SEISMIC_CHANNEL_COUNT);

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
}


int main(void)
{
    test_valid_geometry_round_trips();
    test_missing_geometry_key_defaults_to_zeroed();
    test_malformed_pod_entry_is_skipped_not_fatal();
    test_missing_file_is_a_real_error();
    test_unparseable_json_is_a_real_error();
    test_rejects_null_arguments();
    test_v2_writer_and_participation_reader_round_trip();
    test_v1_participation_defaults();
    test_v2_malformed_pod_values_are_isolated();
    test_unsupported_schema_versions_are_rejected();
    test_summary_reader_accepts_v1_v2_and_rejects_unsupported_versions();
    puts("seismic recorder session geometry tests passed");
    return 0;
}
