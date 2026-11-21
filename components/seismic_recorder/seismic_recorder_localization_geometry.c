#include "seismic_recorder_localization_geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "seismic_recorder.h"
#include "seismic_recorder_internal.h"
#include "scan_artifact_path.h"
#include "storage_manager.h"

#define REGISTERED_POSITION_LIMIT_MM 1000000.0

static bool whole_mm(const cJSON *value, int32_t *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || floor(value->valuedouble) != value->valuedouble ||
        fabs(value->valuedouble) > REGISTERED_POSITION_LIMIT_MM) {
        return false;
    }
    *out = (int32_t)value->valuedouble;
    return true;
}

static bool small_id(const cJSON *value, unsigned *out)
{
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || floor(value->valuedouble) != value->valuedouble ||
        value->valuedouble < 0.0 || value->valuedouble > 255.0) {
        return false;
    }
    *out = (unsigned)value->valuedouble;
    return true;
}

esp_err_t seismic_recorder_registered_positions_parse(const char *json, seismic_recorder_registered_positions_t *out)
{
    if (json == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *block = cJSON_GetObjectItemCaseSensitive(root, "geophone_positions");
    if (block == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    seismic_recorder_registered_positions_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(block, "source");
    const cJSON *frame = cJSON_GetObjectItemCaseSensitive(block, "frame");
    const cJSON *positions = cJSON_GetObjectItemCaseSensitive(block, "positions");
    const cJSON *bound = cJSON_GetObjectItemCaseSensitive(root, "geometry_computed_at_ms");
    bool ok = cJSON_IsObject(block) &&
        cJSON_IsString(source) && source->valuestring != NULL && strcmp(source->valuestring, "APRILTAG_CENTER_APPROX") == 0 &&
        cJSON_IsString(frame) && frame->valuestring != NULL && strcmp(frame->valuestring, "project_z_up_mm") == 0 &&
        cJSON_IsArray(positions) && cJSON_GetArraySize(positions) >= 1 &&
        cJSON_GetArraySize(positions) <= OBJECT_STORE_POD_COUNT &&
        cJSON_IsNumber(bound) && isfinite(bound->valuedouble) && bound->valuedouble >= 0.0 &&
        floor(bound->valuedouble) == bound->valuedouble && bound->valuedouble <= 9007199254740991.0;
    if (ok) {
        parsed.geometry_computed_at_ms = (uint64_t)bound->valuedouble;
        for (int index = 0; index < cJSON_GetArraySize(positions); ++index) {
            const cJSON *entry = cJSON_GetArrayItem(positions, index);
            unsigned node_id = 0U, tag_id = 0U;
            seismic_recorder_registered_position_t position = {.valid = true};
            /* G<k> is node k on mux channel k - 1, and its tag is tag k: the node, the tag and the pod agree or it is refused. */
            if (!cJSON_IsObject(entry) ||
                !small_id(cJSON_GetObjectItemCaseSensitive(entry, "node_id"), &node_id) || node_id < 1U ||
                node_id > OBJECT_STORE_POD_COUNT ||
                !small_id(cJSON_GetObjectItemCaseSensitive(entry, "tag_id"), &tag_id) || tag_id != node_id ||
                !whole_mm(cJSON_GetObjectItemCaseSensitive(entry, "x_mm"), &position.x_mm) ||
                !whole_mm(cJSON_GetObjectItemCaseSensitive(entry, "y_mm"), &position.y_mm) ||
                !whole_mm(cJSON_GetObjectItemCaseSensitive(entry, "z_mm"), &position.z_mm) ||
                parsed.nodes[node_id - 1U].valid) {
                ok = false;
                break;
            }
            position.node_id = (uint8_t)node_id;
            position.tag_id = (uint8_t)tag_id;
            parsed.nodes[node_id - 1U] = position;
        }
    }
    cJSON_Delete(root);
    if (!ok) return ESP_ERR_INVALID_RESPONSE;
    parsed.available = true;
    *out = parsed;
    return ESP_OK;
}

esp_err_t seismic_recorder_read_session_tag_layout(const char *object_id, const char *session_id, seismic_recorder_tag_layout_t *layout)
{
    if (object_id == NULL || session_id == NULL || layout == NULL) return ESP_ERR_INVALID_ARG;
    memset(layout, 0, sizeof(*layout));
    /* The file may have moved from staging into the shared scan of this placement: resolve it like every other scan artifact. */
    char path[288];
    if (scan_artifact_resolve(STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, SEISMIC_RECORDER_TAG_POSITIONS_FILE, path, sizeof(path)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = ESP_OK;
    char *text = malloc(SEISMIC_RECORDER_TAG_POSITIONS_MAX_BYTES + 1U);
    if (text == NULL) return ESP_ERR_NO_MEM;
    err = storage_manager_read_text(path, text, SEISMIC_RECORDER_TAG_POSITIONS_MAX_BYTES + 1U);
    if (err == ESP_OK) err = seismic_recorder_tag_layout_parse(text, layout);
    free(text);
    return err;
}

static void note_status(esp_err_t *registered_status, esp_err_t value)
{
    if (registered_status != NULL) *registered_status = value;
}

esp_err_t seismic_recorder_read_session_localization_geometry(
    const char *object_id,
    const char *session_id,
    object_store_geometry_t *geometry,
    seismic_recorder_localization_geometry_source_t *source,
    esp_err_t *registered_status
)
{
    if (object_id == NULL || session_id == NULL || geometry == NULL) return ESP_ERR_INVALID_ARG;
    if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_CHANNEL_SOUNDING;
    note_status(registered_status, ESP_OK);

    esp_err_t err = seismic_recorder_read_session_geometry(object_id, session_id, geometry);
    if (err != ESP_OK) return err;

    /* From here on the Channel Sounding layout is the answer unless the positions can be used in full. */
    seismic_recorder_session_participation_t participation;
    err = seismic_recorder_read_session_participation(object_id, session_id, &participation);
    if (err != ESP_OK) {
        note_status(registered_status, err);
        return ESP_OK;
    }

    /*
     * The phone's tag layout, if it has one for this session and it can be used in full (MAIN, at least three tagged remotes, not fewer than the
     * Channel Sounding layout has): it is then the geometry of this computation, for every participating node. The stored Channel Sounding /
     * barometer layout is not touched; if the layout cannot be used, the answer is exactly what it was before.
     */
    seismic_recorder_tag_layout_t tag_layout;
    if (seismic_recorder_read_session_tag_layout(object_id, session_id, &tag_layout) == ESP_OK) {
        object_store_geometry_t from_tags;
        if (seismic_recorder_geometry_apply_tag_layout(&participation, &tag_layout, geometry, &from_tags, NULL) == ESP_OK) {
            *geometry = from_tags;
            if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_LAYOUT;
            return ESP_OK;
        }
    }
    if (!participation.geometry_provenance.available || !participation.geometry_provenance.anchor_known ||
        strcmp(participation.geometry_provenance.topology_mode, "remote_only") != 0) {
        note_status(registered_status, ESP_ERR_INVALID_STATE); /* not a REMOTE_ONLY session */
        return ESP_OK;
    }

    char session_path[256];
    char path[288];
    err = seismic_recorder_fs_build_session_path(object_id, session_id, session_path, sizeof(session_path));
    /* The session's own file first, else the shared scan's (a recorded session holds only a scan-ref; issue #27). */
    if (err == ESP_OK &&
        scan_artifact_resolve(STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id, SEISMIC_RECORDER_REGISTRATION_FILE, path, sizeof(path)) != 0) {
        err = ESP_ERR_INVALID_SIZE;
    }
    char *text = err == ESP_OK ? malloc(SEISMIC_RECORDER_REGISTRATION_MAX_BYTES + 1U) : NULL;
    if (err == ESP_OK && text == NULL) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = storage_manager_read_text(path, text, SEISMIC_RECORDER_REGISTRATION_MAX_BYTES + 1U);
    if (err != ESP_OK) {
        free(text);
        note_status(registered_status, err);
        return ESP_OK;
    }

    seismic_recorder_registered_positions_t registered;
    err = seismic_recorder_registered_positions_parse(text, &registered);
    free(text);
    if (err == ESP_OK) {
        object_store_geometry_t candidate = *geometry;
        err = seismic_recorder_geometry_apply_registered_positions(&participation, &registered, &candidate);
        if (err == ESP_OK) {
            *geometry = candidate;
            if (source != NULL) *source = SEISMIC_RECORDER_LOCALIZATION_GEOMETRY_APRILTAG_REGISTERED;
        }
    }
    note_status(registered_status, err);
    return ESP_OK;
}
