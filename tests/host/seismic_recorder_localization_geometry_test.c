#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "seismic_recorder_localization_geometry.h"

/* Fakes for what the reader sits on: the frozen layout, the session metadata and the registration file. */
static object_store_geometry_t s_frozen;
static seismic_recorder_session_participation_t s_participation;
static esp_err_t s_geometry_error;
static esp_err_t s_participation_error;
static char s_file[SEISMIC_RECORDER_REGISTRATION_MAX_BYTES + 1U];
static bool s_file_present;
static esp_err_t s_file_error;
static char s_read_path[256];

esp_err_t seismic_recorder_read_session_geometry(const char *object_id, const char *session_id, object_store_geometry_t *geometry)
{
    (void)object_id; (void)session_id;
    if (s_geometry_error != ESP_OK) return s_geometry_error;
    *geometry = s_frozen;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_participation(
    const char *object_id, const char *session_id, seismic_recorder_session_participation_t *participation)
{
    (void)object_id; (void)session_id;
    if (s_participation_error != ESP_OK) return s_participation_error;
    *participation = s_participation;
    return ESP_OK;
}

esp_err_t seismic_recorder_fs_build_session_path(const char *object_id, const char *session_id, char *path, size_t path_size)
{
    return snprintf(path, path_size, "/objects/%s/sessions/%s", object_id, session_id) < (int)path_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    snprintf(s_read_path, sizeof(s_read_path), "%s", path);
    if (s_file_error != ESP_OK) return s_file_error;
    if (!s_file_present) return ESP_ERR_NOT_FOUND;
    if (strlen(s_file) + 1U > buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, s_file, strlen(s_file) + 1U);
    return ESP_OK;
}

#define GEOMETRY_AT_MS 4000U

static const char *k_good_block =
    "\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":["
    "{\"node_id\":1,\"tag_id\":1,\"x_mm\":100,\"y_mm\":200,\"z_mm\":20},"
    "{\"node_id\":2,\"tag_id\":2,\"x_mm\":1100,\"y_mm\":200,\"z_mm\":20},"
    "{\"node_id\":3,\"tag_id\":3,\"x_mm\":1100,\"y_mm\":1200,\"z_mm\":20},"
    "{\"node_id\":4,\"tag_id\":4,\"x_mm\":100,\"y_mm\":1200,\"z_mm\":20}]}";

static void set_file(unsigned long long geometry_at_ms, const char *block)
{
    snprintf(s_file, sizeof(s_file), "{\"schema\":1,\"scan_frame_id\":null,\"valid\":false,\"geometry_computed_at_ms\":%llu%s%s}",
        geometry_at_ms, block != NULL ? "," : "", block != NULL ? block : "");
    s_file_present = true;
}

static void reset_world(unsigned pods, bool remote_only)
{
    memset(&s_frozen, 0, sizeof(s_frozen));
    memset(&s_participation, 0, sizeof(s_participation));
    s_geometry_error = s_participation_error = s_file_error = ESP_OK;
    s_file_present = false;
    s_file[0] = '\0';
    s_read_path[0] = '\0';
    for (unsigned i = 0U; i < pods; ++i) {
        s_frozen.pods[i] = (object_store_pod_geometry_t){
            .computed = true, .leveled = true, .x_mm = (int32_t)(7 * i), .y_mm = (int32_t)(9 * i), .residual_mm = 40,
            .uncertainty_mm = 150, .computed_at_ms = GEOMETRY_AT_MS,
        };
        s_participation.pods[i] = (seismic_recorder_pod_participation_t){
            .assigned = true, .node_id_known = true, .node_id = (uint8_t)(i + 1U),
        };
    }
    s_participation.geometry_provenance.available = true;
    if (remote_only) {
        snprintf(s_participation.geometry_provenance.topology_mode, sizeof(s_participation.geometry_provenance.topology_mode),
            "remote_only");
        s_participation.geometry_provenance.anchor_known = true;
        s_participation.geometry_provenance.anchor_node_id = 1U;
    }
}

static esp_err_t read_geometry(object_store_geometry_t *geometry, seismic_recorder_localization_geometry_source_t *source,
                               esp_err_t *status)
{
    return seismic_recorder_read_session_localization_geometry("OBJ_0001", "S000001", geometry, source, status);
}

static void registered_positions_parse_accepts_the_artifact_block(void)
{
    char json[1024];
    seismic_recorder_registered_positions_t registered;
    snprintf(json, sizeof(json), "{\"schema\":1,\"geometry_computed_at_ms\":%u,%s}", GEOMETRY_AT_MS, k_good_block);
    assert(seismic_recorder_registered_positions_parse(json, &registered) == ESP_OK);
    assert(registered.available && registered.geometry_computed_at_ms == GEOMETRY_AT_MS);
    for (unsigned i = 0U; i < 4U; ++i) {
        assert(registered.nodes[i].valid && registered.nodes[i].node_id == i + 1U && registered.nodes[i].tag_id == i + 1U);
    }
    assert(registered.nodes[2].x_mm == 1100 && registered.nodes[2].y_mm == 1200 && registered.nodes[2].z_mm == 20);

    /* An ordinary registration has no block: not an error, just nothing to use. */
    assert(seismic_recorder_registered_positions_parse("{\"schema\":1,\"geometry_computed_at_ms\":4000}", &registered) ==
           ESP_ERR_NOT_FOUND);
    assert(!registered.available);
}

static void registered_positions_parse_refuses_a_broken_contract(void)
{
    static const struct { const char *name; const char *text; } bad[] = {
        {"unknown source", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"GPS\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"unknown frame", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"webxr_local_floor\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"no binding", "{\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"node 5", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":5,\"tag_id\":5,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"tag of another node", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":2,\"tag_id\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"node repeated", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0},{\"node_id\":1,\"tag_id\":1,\"x_mm\":9,\"y_mm\":9,\"z_mm\":9}]}}"},
        {"fraction", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":0.5,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"too far", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[{\"node_id\":1,\"tag_id\":1,\"x_mm\":2000000,\"y_mm\":0,\"z_mm\":0}]}}"},
        {"no positions", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":{\"source\":\"APRILTAG_CENTER_APPROX\",\"frame\":\"project_z_up_mm\",\"positions\":[]}}"},
        {"not an object", "{\"geometry_computed_at_ms\":1,\"geophone_positions\":7}"},
        {"not json", "{{{"},
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        seismic_recorder_registered_positions_t registered;
        memset(&registered, 0xAB, sizeof(registered));
        const esp_err_t err = seismic_recorder_registered_positions_parse(bad[i].text, &registered);
        if (err != ESP_ERR_INVALID_RESPONSE) { printf("case '%s' returned %d\n", bad[i].name, (int)err); }
        assert(err == ESP_ERR_INVALID_RESPONSE);
        assert(!registered.available); /* nothing partially accepted */
    }
    assert(seismic_recorder_registered_positions_parse(NULL, &(seismic_recorder_registered_positions_t){0}) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_registered_positions_parse("{}", NULL) == ESP_ERR_INVALID_ARG);
}

static void localization_geometry_uses_registered_positions_for_remote_only(void)
{
    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    esp_err_t status = ESP_FAIL;
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED && status == ESP_OK);
    assert(strcmp(s_read_path, "/objects/OBJ_0001/sessions/S000001/registration-v1.json") == 0);
    assert(geometry.anchored);
    for (unsigned i = 0U; i < 4U; ++i) assert(geometry.pods[i].computed && geometry.pods[i].z_mm == 20);
    assert(geometry.pods[0].x_mm == 100 && geometry.pods[2].y_mm == 1200);
}

static void localization_geometry_keeps_channel_sounding_when_positions_do_not_apply(void)
{
    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t source;
    esp_err_t status;
    object_store_geometry_t frozen_layout;

    /* No registration file at all. */
    reset_world(4U, true);
    frozen_layout = s_frozen;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_NOT_FOUND);
    assert(memcmp(&geometry, &frozen_layout, sizeof(geometry)) == 0);

    /* A registration without the block (the MAIN-based kind). */
    set_file(GEOMETRY_AT_MS, NULL);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_NOT_FOUND);

    /* A legacy (MAIN present) session never takes them, whatever the file holds. */
    reset_world(4U, false);
    set_file(GEOMETRY_AT_MS, k_good_block);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_INVALID_STATE);
    assert(!geometry.anchored && geometry.pods[0].x_mm == 0 && geometry.pods[1].x_mm == 7);

    /* Positions made for another geometry (it was recomputed since). */
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS + 1U, k_good_block);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_INVALID_STATE);
    assert(!geometry.anchored);

    /* A block that breaks the contract. */
    reset_world(4U, true);
    snprintf(s_file, sizeof(s_file), "{\"geometry_computed_at_ms\":4000,\"geophone_positions\":{\"source\":\"GPS\",\"frame\":\"project_z_up_mm\",\"positions\":[]}}");
    s_file_present = true;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_INVALID_RESPONSE);

    /* A read error of the file is also "not usable", reported, not fatal. */
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    s_file_error = ESP_ERR_INVALID_SIZE;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status == ESP_ERR_INVALID_SIZE);

    /* Metadata that cannot be read means the session kind is unknown: the layout stays. */
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    s_participation_error = ESP_ERR_NOT_FOUND;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && status != ESP_OK);

    /* Only a failure to read the layout itself is an error. */
    reset_world(4U, true);
    s_geometry_error = ESP_ERR_NOT_FOUND;
    assert(read_geometry(&geometry, &source, &status) == ESP_ERR_NOT_FOUND);
    assert(read_geometry(NULL, &source, &status) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_localization_geometry(NULL, "S1", &geometry, &source, &status) == ESP_ERR_INVALID_ARG);
}

/* No argument runs everything; one argument runs the named case (to show a single RED/GREEN case). */
int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"registered_positions_parse_accepts_the_artifact_block", registered_positions_parse_accepts_the_artifact_block},
        {"registered_positions_parse_refuses_a_broken_contract", registered_positions_parse_refuses_a_broken_contract},
        {"localization_geometry_uses_registered_positions_for_remote_only",
         localization_geometry_uses_registered_positions_for_remote_only},
        {"localization_geometry_keeps_channel_sounding_when_positions_do_not_apply",
         localization_geometry_keeps_channel_sounding_when_positions_do_not_apply},
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
