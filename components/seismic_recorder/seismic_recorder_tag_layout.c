#include "seismic_recorder_tag_layout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define TAG_LAYOUT_COORDINATE_LIMIT_MM 200000.0
#define TAG_LAYOUT_UNCERTAINTY_LIMIT_MM 100000.0
#define TAG_LAYOUT_COUNT_LIMIT 100000.0

static bool whole_in_range(const cJSON *value, double min, double max, double *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || floor(value->valuedouble) != value->valuedouble ||
        value->valuedouble < min || value->valuedouble > max) {
        return false;
    }
    *out = value->valuedouble;
    return true;
}

static bool only_keys(const cJSON *object, const char *const *allowed, size_t allowed_count)
{
    for (const cJSON *child = object->child; child != NULL; child = child->next) {
        bool known = false;
        for (size_t i = 0U; i < allowed_count && child->string != NULL; ++i) {
            if (strcmp(child->string, allowed[i]) == 0) { known = true; break; }
        }
        if (!known) return false;
    }
    return true;
}

static bool scan_id_valid(const char *text)
{
    const size_t length = strlen(text);
    if (length == 0U || length >= SEISMIC_RECORDER_TAG_LAYOUT_SCAN_ID_MAX) return false;
    for (size_t i = 0U; i < length; ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '-')) return false;
    }
    return true;
}

esp_err_t seismic_recorder_tag_layout_parse(const char *json, seismic_recorder_tag_layout_t *out)
{
    if (json == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    static const char *const root_keys[] = {"schema", "kind", "frame", "scan_frame_id", "created_at_ms", "geometry_computed_at_ms", "main_scan_position_mm",
        "weakest_main_triangle_height_mm", "nodes"};
    static const char *const node_keys[] = {"node_id", "tag_id", "x_mm", "y_mm", "z_mm", "uncertainty_mm", "confirmed", "observations", "views"};

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    seismic_recorder_tag_layout_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    bool ok = cJSON_IsObject(root) && only_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]));
    double number = 0.0;

    const cJSON *schema = ok ? cJSON_GetObjectItemCaseSensitive(root, "schema") : NULL;
    const cJSON *kind = ok ? cJSON_GetObjectItemCaseSensitive(root, "kind") : NULL;
    const cJSON *frame = ok ? cJSON_GetObjectItemCaseSensitive(root, "frame") : NULL;
    ok = ok && whole_in_range(schema, 1.0, 1.0, &number) &&
        cJSON_IsString(kind) && kind->valuestring != NULL && strcmp(kind->valuestring, "tag_positions") == 0 &&
        cJSON_IsString(frame) && frame->valuestring != NULL && strcmp(frame->valuestring, "sensor_main_origin_z_up_mm") == 0;

    const cJSON *scan_id = ok ? cJSON_GetObjectItemCaseSensitive(root, "scan_frame_id") : NULL;
    if (ok) {
        if (cJSON_IsNull(scan_id)) {
            parsed.scan_frame_id[0] = '\0';
        } else if (cJSON_IsString(scan_id) && scan_id->valuestring != NULL && scan_id_valid(scan_id->valuestring)) {
            snprintf(parsed.scan_frame_id, sizeof(parsed.scan_frame_id), "%s", scan_id->valuestring);
        } else {
            ok = false;
        }
    }
    if (ok && whole_in_range(cJSON_GetObjectItemCaseSensitive(root, "created_at_ms"), 0.0, 9007199254740991.0, &number)) {
        parsed.created_at_ms = (uint64_t)number;
    } else {
        ok = false;
    }
    /* Optional: absent or null means the placement epoch was not recorded; such a layout is never used for localization (issue #26). */
    const cJSON *epoch = ok ? cJSON_GetObjectItemCaseSensitive(root, "geometry_computed_at_ms") : NULL;
    if (ok && epoch != NULL && !cJSON_IsNull(epoch)) {
        if (whole_in_range(epoch, 1.0, 9007199254740991.0, &number)) parsed.geometry_computed_at_ms = (uint64_t)number;
        else ok = false;
    }
    const cJSON *main_position = ok ? cJSON_GetObjectItemCaseSensitive(root, "main_scan_position_mm") : NULL;
    ok = ok && cJSON_IsArray(main_position) && cJSON_GetArraySize(main_position) == 3;
    for (int axis = 0; ok && axis < 3; ++axis) {
        if (whole_in_range(cJSON_GetArrayItem(main_position, axis), -TAG_LAYOUT_COORDINATE_LIMIT_MM, TAG_LAYOUT_COORDINATE_LIMIT_MM, &number)) {
            parsed.main_scan_mm[axis] = (int32_t)number;
        } else {
            ok = false;
        }
    }
    if (ok && whole_in_range(cJSON_GetObjectItemCaseSensitive(root, "weakest_main_triangle_height_mm"), 0.0, 2.0 * TAG_LAYOUT_COORDINATE_LIMIT_MM, &number)) {
        parsed.weakest_main_triangle_height_mm = (int32_t)number;
    } else {
        ok = false;
    }

    const cJSON *nodes = ok ? cJSON_GetObjectItemCaseSensitive(root, "nodes") : NULL;
    ok = ok && cJSON_IsArray(nodes) && cJSON_GetArraySize(nodes) >= 3 && cJSON_GetArraySize(nodes) <= (int)SEISMIC_RECORDER_TAG_LAYOUT_NODE_COUNT;
    for (int index = 0; ok && index < cJSON_GetArraySize(nodes); ++index) {
        const cJSON *entry = cJSON_GetArrayItem(nodes, index);
        double node_id = 0.0, tag_id = 0.0, x = 0.0, y = 0.0, z = 0.0, uncertainty = 0.0, observations = 0.0, views = 0.0;
        const cJSON *confirmed = NULL;
        ok = cJSON_IsObject(entry) && only_keys(entry, node_keys, sizeof(node_keys) / sizeof(node_keys[0])) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "node_id"), 0.0, 4.0, &node_id) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "tag_id"), 0.0, 4.0, &tag_id) && tag_id == node_id &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "x_mm"), -TAG_LAYOUT_COORDINATE_LIMIT_MM, TAG_LAYOUT_COORDINATE_LIMIT_MM, &x) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "y_mm"), -TAG_LAYOUT_COORDINATE_LIMIT_MM, TAG_LAYOUT_COORDINATE_LIMIT_MM, &y) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "z_mm"), -TAG_LAYOUT_COORDINATE_LIMIT_MM, TAG_LAYOUT_COORDINATE_LIMIT_MM, &z) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "uncertainty_mm"), 0.0, TAG_LAYOUT_UNCERTAINTY_LIMIT_MM, &uncertainty) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "observations"), 0.0, TAG_LAYOUT_COUNT_LIMIT, &observations) &&
            whole_in_range(cJSON_GetObjectItemCaseSensitive(entry, "views"), 0.0, TAG_LAYOUT_COUNT_LIMIT, &views);
        confirmed = ok ? cJSON_GetObjectItemCaseSensitive(entry, "confirmed") : NULL;
        ok = ok && cJSON_IsBool(confirmed) && !parsed.nodes[(size_t)node_id].valid;
        if (!ok) break;
        parsed.nodes[(size_t)node_id] = (seismic_recorder_tag_node_t){
            .valid = true, .node_id = (uint8_t)node_id, .tag_id = (uint8_t)tag_id, .x_mm = (int32_t)x, .y_mm = (int32_t)y, .z_mm = (int32_t)z,
            .uncertainty_mm = (int32_t)uncertainty, .confirmed = cJSON_IsTrue(confirmed), .observations = (uint32_t)observations, .views = (uint32_t)views,
        };
        ++parsed.node_count;
    }
    cJSON_Delete(root);

    /* MAIN is the origin; no two sensors (MAIN included) may share a place. */
    ok = ok && parsed.nodes[0].valid && parsed.nodes[0].x_mm == 0 && parsed.nodes[0].y_mm == 0 && parsed.nodes[0].z_mm == 0 &&
        parsed.nodes[0].uncertainty_mm == 0;
    for (size_t a = 0U; ok && a < SEISMIC_RECORDER_TAG_LAYOUT_NODE_COUNT; ++a) {
        if (!parsed.nodes[a].valid) continue;
        for (size_t b = a + 1U; ok && b < SEISMIC_RECORDER_TAG_LAYOUT_NODE_COUNT; ++b) {
            if (!parsed.nodes[b].valid) continue;
            const double dx = (double)parsed.nodes[a].x_mm - parsed.nodes[b].x_mm;
            const double dy = (double)parsed.nodes[a].y_mm - parsed.nodes[b].y_mm;
            const double dz = (double)parsed.nodes[a].z_mm - parsed.nodes[b].z_mm;
            if (dx * dx + dy * dy + dz * dz < 100.0) ok = false;
        }
    }
    if (!ok) return ESP_ERR_INVALID_RESPONSE;
    parsed.available = true;
    *out = parsed;
    return ESP_OK;
}
