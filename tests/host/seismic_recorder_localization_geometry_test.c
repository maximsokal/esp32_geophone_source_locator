#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define test_mkdir(path) _mkdir(path)
#define test_rmdir(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_mkdir(path) mkdir(path, 0775)
#define test_rmdir(path) rmdir(path)
#endif

#include "seismic_recorder_localization_geometry.h"
#include "storage_manager.h"

/* Fakes for what the reader sits on: the frozen layout, the session metadata and the registration file. */
static object_store_geometry_t s_frozen;
static seismic_recorder_session_participation_t s_participation;
static esp_err_t s_geometry_error;
static esp_err_t s_participation_error;
static char s_file[SEISMIC_RECORDER_REGISTRATION_MAX_BYTES + 1U];
static bool s_file_present;
static esp_err_t s_file_error;
static char s_read_path[256];
static char s_tag_path[256];
static char s_tag_file[SEISMIC_RECORDER_TAG_POSITIONS_MAX_BYTES + 1U];
static bool s_tag_present;
static esp_err_t s_tag_error;
static unsigned s_tag_reads;

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
    if (strstr(path, SEISMIC_RECORDER_TAG_POSITIONS_FILE) != NULL) {
        snprintf(s_tag_path, sizeof(s_tag_path), "%s", path);
        ++s_tag_reads;
        if (s_tag_error != ESP_OK) return s_tag_error;
        if (!s_tag_present) return ESP_ERR_NOT_FOUND;
        if (strlen(s_tag_file) + 1U > buffer_size) return ESP_ERR_INVALID_SIZE;
        memcpy(buffer, s_tag_file, strlen(s_tag_file) + 1U);
        return ESP_OK;
    }
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
    s_tag_path[0] = '\0';
    s_tag_file[0] = '\0';
    s_tag_present = false;
    s_tag_error = ESP_OK;
    s_tag_reads = 0U;
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
    assert(strcmp(s_read_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json") == 0);
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

/* ------------------------------------------------------------------ tag layout (tag-positions-v1.json) */

static const char *k_tag_nodes_all =
    "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":9,\"views\":4},"
    "{\"node_id\":1,\"tag_id\":1,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":8,\"views\":4},"
    "{\"node_id\":2,\"tag_id\":2,\"x_mm\":0,\"y_mm\":800,\"z_mm\":0,\"uncertainty_mm\":6,\"confirmed\":false,\"observations\":3,\"views\":2},"
    "{\"node_id\":3,\"tag_id\":3,\"x_mm\":-100,\"y_mm\":500,\"z_mm\":200,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":8,\"views\":4},"
    "{\"node_id\":4,\"tag_id\":4,\"x_mm\":900,\"y_mm\":900,\"z_mm\":50,\"uncertainty_mm\":7,\"confirmed\":true,\"observations\":7,\"views\":3}";

#define NODE(id, tag, x, y, z, unc, conf, obs, views) "{\"node_id\":" id ",\"tag_id\":" tag ",\"x_mm\":" x ",\"y_mm\":" y ",\"z_mm\":" z \
    ",\"uncertainty_mm\":" unc ",\"confirmed\":" conf ",\"observations\":" obs ",\"views\":" views "}"

/* A tag file with every part replaceable; NULL keeps the valid default. */
static void make_tag_json(char *out, size_t size, const char *root_extra, const char *schema, const char *kind, const char *frame, const char *scan_id,
                          const char *created, const char *main_position, const char *weakest, const char *nodes)
{
    snprintf(out, size,
        "{%s\"schema\":%s,\"kind\":%s,\"frame\":%s,\"scan_frame_id\":%s,\"created_at_ms\":%s,\"main_scan_position_mm\":%s,"
        "\"weakest_main_triangle_height_mm\":%s,\"nodes\":[%s]}",
        root_extra != NULL ? root_extra : "",
        schema != NULL ? schema : "1",
        kind != NULL ? kind : "\"tag_positions\"",
        frame != NULL ? frame : "\"sensor_main_origin_z_up_mm\"",
        scan_id != NULL ? scan_id : "\"scan-crc32:732740d9\"",
        created != NULL ? created : "1700000000000",
        main_position != NULL ? main_position : "[1000,2000,1000]",
        weakest != NULL ? weakest : "250",
        nodes != NULL ? nodes : k_tag_nodes_all);
}

static void set_tag_file_text(const char *text)
{
    snprintf(s_tag_file, sizeof(s_tag_file), "%s", text);
    s_tag_present = true;
}

static void set_good_tag_file(void)
{
    char json[2048];
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    set_tag_file_text(json);
}

static void tag_layout_parse_accepts_the_artifact(void)
{
    char json[2048];
    seismic_recorder_tag_layout_t layout;
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    assert(seismic_recorder_tag_layout_parse(json, &layout) == ESP_OK);
    assert(layout.available && layout.node_count == 5U);
    assert(strcmp(layout.scan_frame_id, "scan-crc32:732740d9") == 0);
    assert(layout.created_at_ms == 1700000000000ULL);
    assert(layout.main_scan_mm[0] == 1000 && layout.main_scan_mm[1] == 2000 && layout.main_scan_mm[2] == 1000);
    assert(layout.weakest_main_triangle_height_mm == 250);
    assert(layout.nodes[0].valid && layout.nodes[0].x_mm == 0 && layout.nodes[0].uncertainty_mm == 0);
    assert(layout.nodes[1].valid && layout.nodes[1].x_mm == 500 && layout.nodes[1].confirmed && layout.nodes[1].observations == 8U && layout.nodes[1].views == 4U);
    assert(layout.nodes[2].valid && layout.nodes[2].y_mm == 800 && !layout.nodes[2].confirmed && layout.nodes[2].uncertainty_mm == 6);
    assert(layout.nodes[3].x_mm == -100 && layout.nodes[3].z_mm == 200);
    assert(layout.nodes[4].tag_id == 4U && layout.nodes[4].node_id == 4U);

    /* MAIN plus two remotes is the smallest layout (a triangle); a null scan id means the phone did not know the scan yet. */
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, "null", NULL, NULL, NULL,
        "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":1,\"views\":1},"
        "{\"node_id\":1,\"tag_id\":1,\"x_mm\":500,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":true,\"observations\":1,\"views\":1},"
        "{\"node_id\":3,\"tag_id\":3,\"x_mm\":0,\"y_mm\":800,\"z_mm\":0,\"uncertainty_mm\":5,\"confirmed\":false,\"observations\":1,\"views\":1}");
    assert(seismic_recorder_tag_layout_parse(json, &layout) == ESP_OK);
    assert(layout.node_count == 3U && layout.scan_frame_id[0] == '\0' && !layout.nodes[2].valid && layout.nodes[3].valid);
}

static void tag_layout_parse_refuses_a_broken_contract(void)
{
    static const struct { const char *name; const char *root_extra, *schema, *kind, *frame, *scan_id, *created, *main_position, *weakest, *nodes; } bad[] = {
        {"unknown root field", "\"extra\":1,", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
        {"schema 2", NULL, "2", NULL, NULL, NULL, NULL, NULL, NULL, NULL},
        {"schema as text", NULL, "\"1\"", NULL, NULL, NULL, NULL, NULL, NULL, NULL},
        {"wrong kind", NULL, NULL, "\"registration\"", NULL, NULL, NULL, NULL, NULL, NULL},
        {"kind not text", NULL, NULL, "7", NULL, NULL, NULL, NULL, NULL, NULL},
        {"wrong frame", NULL, NULL, NULL, "\"webxr_local_floor\"", NULL, NULL, NULL, NULL, NULL},
        {"scan id with a path", NULL, NULL, NULL, NULL, "\"../x\"", NULL, NULL, NULL, NULL},
        {"scan id empty", NULL, NULL, NULL, NULL, "\"\"", NULL, NULL, NULL, NULL},
        {"scan id too long", NULL, NULL, NULL, NULL, "\"scan-crc32:0123456789012345678901234567890123456789012345\"", NULL, NULL, NULL, NULL},
        {"scan id a number", NULL, NULL, NULL, NULL, "5", NULL, NULL, NULL, NULL},
        {"created fraction", NULL, NULL, NULL, NULL, NULL, "1.5", NULL, NULL, NULL},
        {"created negative", NULL, NULL, NULL, NULL, NULL, "-1", NULL, NULL, NULL},
        {"main position two values", NULL, NULL, NULL, NULL, NULL, NULL, "[1,2]", NULL, NULL},
        {"main position fraction", NULL, NULL, NULL, NULL, NULL, NULL, "[1,2.5,3]", NULL, NULL},
        {"main position out of range", NULL, NULL, NULL, NULL, NULL, NULL, "[1,2,300000]", NULL, NULL},
        {"main position text", NULL, NULL, NULL, NULL, NULL, NULL, "\"1,2,3\"", NULL, NULL},
        {"weakest negative", NULL, NULL, NULL, NULL, NULL, NULL, NULL, "-1", NULL},
        {"weakest fraction", NULL, NULL, NULL, NULL, NULL, NULL, NULL, "2.5", NULL},
    };
#define BAD_CASES (sizeof(bad) / sizeof(bad[0]))
    char json[2048];
    seismic_recorder_tag_layout_t layout;
    for (size_t i = 0U; i < BAD_CASES; ++i) {
        make_tag_json(json, sizeof(json), bad[i].root_extra, bad[i].schema, bad[i].kind, bad[i].frame, bad[i].scan_id, bad[i].created, bad[i].main_position, bad[i].weakest, bad[i].nodes);
        memset(&layout, 0xAB, sizeof(layout));
        const esp_err_t err = seismic_recorder_tag_layout_parse(json, &layout);
        if (err != ESP_ERR_INVALID_RESPONSE) printf("case '%s' returned %d\n", bad[i].name, (int)err);
        assert(err == ESP_ERR_INVALID_RESPONSE);
        assert(!layout.available && layout.node_count == 0U); /* nothing partially accepted */
    }

    /* Node-level breaks, each replacing the whole node list. */
    static const struct { const char *name; const char *nodes; } bad_nodes[] = {
        {"two nodes only", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1")},
        {"no MAIN", NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1") "," NODE("3", "3", "-100", "500", "200", "5", "true", "1", "1")},
        {"unknown node field", "{\"node_id\":0,\"tag_id\":0,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"uncertainty_mm\":0,\"confirmed\":true,\"observations\":1,\"views\":1,\"extra\":1},"
            NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"node id 5", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("5", "5", "0", "800", "0", "5", "true", "1", "1")},
        {"node id repeated", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("1", "1", "0", "800", "0", "5", "true", "1", "1")},
        {"tag of another node", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "3", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"x fraction", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500.5", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"y out of range", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "999999", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"z as text", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "\"0\"", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"negative uncertainty", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "-5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"confirmed as a number", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "1", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"negative observations", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "-1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"MAIN not at the origin", NODE("0", "0", "1", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"MAIN with uncertainty", NODE("0", "0", "0", "0", "0", "3", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1")},
        {"two nodes 9 mm apart", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "509", "0", "0", "5", "true", "1", "1")},
        {"node on top of MAIN", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "3", "4", "0", "5", "true", "1", "1")},
        {"six nodes", NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "0", "800", "0", "5", "true", "1", "1") ","
            NODE("3", "3", "-100", "500", "200", "5", "true", "1", "1") "," NODE("4", "4", "900", "900", "50", "5", "true", "1", "1") "," NODE("4", "4", "950", "950", "50", "5", "true", "1", "1")},
    };
    for (size_t i = 0U; i < sizeof(bad_nodes) / sizeof(bad_nodes[0]); ++i) {
        make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, bad_nodes[i].nodes);
        memset(&layout, 0xAB, sizeof(layout));
        const esp_err_t err = seismic_recorder_tag_layout_parse(json, &layout);
        if (err != ESP_ERR_INVALID_RESPONSE) printf("node case '%s' returned %d\n", bad_nodes[i].name, (int)err);
        assert(err == ESP_ERR_INVALID_RESPONSE);
        assert(!layout.available);
    }
    /* Exactly 10 mm apart is still a layout. */
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NODE("0", "0", "0", "0", "0", "0", "true", "1", "1") "," NODE("1", "1", "500", "0", "0", "5", "true", "1", "1") "," NODE("2", "2", "510", "0", "0", "5", "true", "1", "1"));
    assert(seismic_recorder_tag_layout_parse(json, &layout) == ESP_OK);

    assert(seismic_recorder_tag_layout_parse("{{{", &layout) == ESP_ERR_INVALID_RESPONSE && !layout.available);
    assert(seismic_recorder_tag_layout_parse("[]", &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(seismic_recorder_tag_layout_parse("7", &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(seismic_recorder_tag_layout_parse(NULL, &layout) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_tag_layout_parse(json, NULL) == ESP_ERR_INVALID_ARG);
}

static void the_tag_layout_is_read_from_the_session_directory(void)
{
    seismic_recorder_tag_layout_t layout;
    reset_world(4U, false);
    assert(seismic_recorder_read_session_tag_layout("OBJ_0001", "S000001", &layout) == ESP_ERR_NOT_FOUND && !layout.available);
    set_good_tag_file();
    assert(seismic_recorder_read_session_tag_layout("OBJ_0001", "S000001", &layout) == ESP_OK && layout.available && layout.node_count == 5U);
    assert(strcmp(s_tag_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json") == 0);
    set_tag_file_text("{\"schema\":1}");
    assert(seismic_recorder_read_session_tag_layout("OBJ_0001", "S000001", &layout) == ESP_ERR_INVALID_RESPONSE && !layout.available);
    s_tag_error = ESP_ERR_INVALID_SIZE;
    assert(seismic_recorder_read_session_tag_layout("OBJ_0001", "S000001", &layout) == ESP_ERR_INVALID_SIZE);
    assert(seismic_recorder_read_session_tag_layout(NULL, "S1", &layout) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_tag_layout("O", NULL, &layout) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_tag_layout("O", "S", NULL) == ESP_ERR_INVALID_ARG);
}

static void localization_geometry_uses_the_tag_layout_beside_the_stored_one(void)
{
    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    esp_err_t status = ESP_FAIL;

    /* Any session kind can use it: here a legacy (MAIN present) session. */
    reset_world(4U, false);
    set_good_tag_file();
    const object_store_geometry_t stored = s_frozen;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT);
    assert(geometry.anchored);
    assert(geometry.pods[0].computed && geometry.pods[0].x_mm == 500 && geometry.pods[1].y_mm == 800 && geometry.pods[2].z_mm == 200 && geometry.pods[3].x_mm == 900);
    assert(geometry.pods[1].uncertainty_mm == 6 && geometry.pods[0].residual_mm == 0);
    assert(memcmp(&s_frozen, &stored, sizeof(stored)) == 0);   /* the Channel Sounding / barometer layout of the session is untouched */
    assert(strcmp(s_tag_path, STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/tag-positions-v1.json") == 0);

    /* It takes precedence over registered positions of a REMOTE_ONLY session: it is the more complete answer. */
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    set_good_tag_file();
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT && geometry.pods[0].x_mm == 500);
}

static void localization_geometry_stays_exactly_as_before_when_the_tag_layout_cannot_be_used(void)
{
    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t source;
    esp_err_t status;

    /* No tag file. */
    reset_world(4U, false);
    const object_store_geometry_t stored = s_frozen;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && memcmp(&geometry, &stored, sizeof(stored)) == 0);

    /* A file that breaks the contract, a read error, unreadable session metadata. */
    set_tag_file_text("{\"schema\":1,\"kind\":\"tag_positions\"}");
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && memcmp(&geometry, &stored, sizeof(stored)) == 0);
    set_good_tag_file();
    s_tag_error = ESP_ERR_INVALID_SIZE;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK && source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING);
    s_tag_error = ESP_OK;
    s_participation_error = ESP_ERR_NOT_FOUND;
    assert(read_geometry(&geometry, &source, &status) == ESP_OK && source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING);

    /* The layout covers three remotes and the stored layout four: a smaller set would lower the dimensionality, the stored layout stays. */
    reset_world(4U, false);
    char json[2048];
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NODE("0", "0", "0", "0", "0", "0", "true", "9", "4") "," NODE("1", "1", "500", "0", "0", "5", "true", "9", "4") ","
        NODE("2", "2", "0", "800", "0", "5", "true", "9", "4") "," NODE("3", "3", "-100", "500", "200", "5", "true", "9", "4"));
    set_tag_file_text(json);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING && !geometry.anchored);

    /* The same three remotes serve when the stored layout also has only three. */
    reset_world(3U, false);
    set_tag_file_text(json);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT && geometry.pods[2].computed && !geometry.pods[3].computed);

    /* MAIN and two remotes is a triangle, but a localization needs three remotes. */
    reset_world(4U, false);
    make_tag_json(json, sizeof(json), NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NODE("0", "0", "0", "0", "0", "0", "true", "9", "4") "," NODE("1", "1", "500", "0", "0", "5", "true", "9", "4") "," NODE("2", "2", "0", "800", "0", "5", "true", "9", "4"));
    set_tag_file_text(json);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK && source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING);

    /* An unusable tag layout does not take the registered positions of a REMOTE_ONLY session away from it. */
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    set_tag_file_text("{\"schema\":1}");
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED && status == ESP_OK);
}

/* No argument runs everything; one argument runs the named case (to show a single RED/GREEN case). */
/* Issue #27: a REMOTE_ONLY registration moved into the shared scan is read through the session's scan-ref, as the HTTP reader does. */
static void write_small_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1U, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void the_registration_is_read_from_the_shared_scan_through_the_scan_ref(void)
{
    const char *session = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001";
    const char *ref = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/scan-ref";
    const char *local = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions/S000001/registration-v1.json";
    const char *shared_dir = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans/SC000001";
    const char *shared = STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans/SC000001/registration-v1.json";
    (void)test_mkdir(STORAGE_MANAGER_MOUNT_POINT);
    (void)test_mkdir(STORAGE_MANAGER_ROOT_PATH);
    (void)test_mkdir(STORAGE_MANAGER_OBJECTS_PATH);
    (void)test_mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
    (void)test_mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)test_mkdir(session);
    (void)test_mkdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans");
    (void)test_mkdir(shared_dir);
    write_small_file(ref, "SC000001");
    write_small_file(shared, "{}");

    object_store_geometry_t geometry;
    seismic_recorder_localization_geometry_source_t source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    esp_err_t status = ESP_FAIL;
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(strcmp(s_read_path, shared) == 0);                       /* the shared file, not a missing session-local one */
    assert(status == ESP_OK && source == SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED);

    /* A session-local registration still wins over the shared one. */
    write_small_file(local, "{}");
    reset_world(4U, true);
    set_file(GEOMETRY_AT_MS, k_good_block);
    assert(read_geometry(&geometry, &source, &status) == ESP_OK);
    assert(strcmp(s_read_path, local) == 0);

    (void)remove(local);
    (void)remove(shared);
    (void)remove(ref);
    (void)test_rmdir(shared_dir);
    (void)test_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/scans");
    (void)test_rmdir(session);
    (void)test_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001/sessions");
    (void)test_rmdir(STORAGE_MANAGER_OBJECTS_PATH "/OBJ_0001");
}

int main(int argc, char **argv)
{
    static const struct { const char *name; void (*run)(void); } cases[] = {
        {"registered_positions_parse_accepts_the_artifact_block", registered_positions_parse_accepts_the_artifact_block},
        {"registered_positions_parse_refuses_a_broken_contract", registered_positions_parse_refuses_a_broken_contract},
        {"localization_geometry_uses_registered_positions_for_remote_only",
         localization_geometry_uses_registered_positions_for_remote_only},
        {"localization_geometry_keeps_channel_sounding_when_positions_do_not_apply",
         localization_geometry_keeps_channel_sounding_when_positions_do_not_apply},
        {"tag_layout_parse_accepts_the_artifact", tag_layout_parse_accepts_the_artifact},
        {"tag_layout_parse_refuses_a_broken_contract", tag_layout_parse_refuses_a_broken_contract},
        {"the_tag_layout_is_read_from_the_session_directory", the_tag_layout_is_read_from_the_session_directory},
        {"localization_geometry_uses_the_tag_layout_beside_the_stored_one", localization_geometry_uses_the_tag_layout_beside_the_stored_one},
        {"localization_geometry_stays_exactly_as_before_when_the_tag_layout_cannot_be_used",
         localization_geometry_stays_exactly_as_before_when_the_tag_layout_cannot_be_used},
        {"the_registration_is_read_from_the_shared_scan_through_the_scan_ref",
         the_registration_is_read_from_the_shared_scan_through_the_scan_ref},
    };
    bool ran = false;
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        ran = true;
    }
    return ran ? 0 : 2;
}
