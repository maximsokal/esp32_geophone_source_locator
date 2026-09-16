#include "object_store_internal.h"

#include <stddef.h>

#include "cJSON.h"
#include "object_format.h"
#include "storage_manager.h"

esp_err_t object_store_json_write_metadata(
    const char *object_id,
    const char *initial_name
)
{
    if (!object_format_id_is_valid(object_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *final_name =
        initial_name != NULL ? initial_name : object_id;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;

    cJSON *schema = cJSON_AddNumberToObject(root, "schema_version", OBJECT_FORMAT_SCHEMA_VERSION);
    cJSON *id = cJSON_AddStringToObject(root, "id", object_id);
    cJSON *name = cJSON_AddStringToObject(root, "name", final_name);
    cJSON *location = cJSON_AddObjectToObject(root, "location");
    if (schema == NULL || id == NULL || name == NULL || location == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    bool location_ok =
        cJSON_AddBoolToObject(location, "valid", false) != NULL &&
        cJSON_AddNumberToObject(location, "latitude", 0.0) != NULL &&
        cJSON_AddNumberToObject(location, "longitude", 0.0) != NULL &&
        cJSON_AddNumberToObject(location, "accuracy_m", 0.0) != NULL &&
        cJSON_AddBoolToObject(location, "altitude_valid", false) != NULL &&
        cJSON_AddNumberToObject(location, "altitude_m", 0.0) != NULL &&
        cJSON_AddNumberToObject(location, "altitude_accuracy_m", 0.0) != NULL &&
        cJSON_AddNumberToObject(location, "captured_at_ms", 0.0) != NULL &&
        cJSON_AddStringToObject(location, "source", "NONE") != NULL;
    if (!location_ok) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    float cable_lengths[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(cable_lengths) != ESP_OK) {
        for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
            cable_lengths[i] = OBJECT_STORE_DEFAULT_CABLE_LENGTH_M;
        }
    }

    cJSON *pods = cJSON_AddArrayToObject(root, "pods");
    if (pods == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *pod = cJSON_CreateObject();
        if (pod == NULL ||
            cJSON_AddNumberToObject(pod, "id", i + 1) == NULL ||
            cJSON_AddNumberToObject(pod, "cable_length_m", cable_lengths[i]) == NULL) {
            cJSON_Delete(pod);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(pods, pod);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;

    char metadata_path[OBJECT_STORE_FS_PATH_SIZE];
    esp_err_t err = object_store_fs_metadata_path(object_id, metadata_path, sizeof(metadata_path));
    if (err == ESP_OK) {
        err = storage_manager_atomic_write_text(metadata_path, json);
    }
    cJSON_free(json);
    return err;
}
