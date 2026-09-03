#include "object_store.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "object_format.h"
#include "object_store_internal.h"
#include "storage_manager.h"

#define OBJECT_STORE_JSON_BUFFER_SIZE 4096
#define OBJECT_STORE_SETTINGS_PATH STORAGE_MANAGER_ROOT_PATH "/settings.json"
#define OBJECT_STORE_MIN_CABLE_LENGTH_M 0.1f
#define OBJECT_STORE_MAX_CABLE_LENGTH_M 100.0f

static void object_store_default_cables(float lengths[OBJECT_STORE_POD_COUNT])
{
    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        lengths[i] = OBJECT_STORE_DEFAULT_CABLE_LENGTH_M;
    }
}

static bool object_store_cable_valid(float length_m)
{
    return isfinite(length_m) &&
        length_m >= OBJECT_STORE_MIN_CABLE_LENGTH_M &&
        length_m <= OBJECT_STORE_MAX_CABLE_LENGTH_M;
}

static esp_err_t object_store_load_metadata_locked(const char *object_id, cJSON **root_out)
{
    if (!object_format_id_is_valid(object_id) || root_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *root_out = NULL;
    char path[OBJECT_STORE_FS_PATH_SIZE];
    esp_err_t err = object_store_fs_metadata_path(object_id, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    char buffer[OBJECT_STORE_JSON_BUFFER_SIZE];
    err = storage_manager_read_text(path, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *root_out = root;
    return ESP_OK;
}

static esp_err_t object_store_write_metadata_locked(const char *object_id, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char path[OBJECT_STORE_FS_PATH_SIZE];
    esp_err_t err = object_store_fs_metadata_path(object_id, path, sizeof(path));
    if (err == ESP_OK) {
        err = storage_manager_atomic_write_text(path, json);
    }
    cJSON_free(json);
    return err;
}

esp_err_t object_store_settings_load_locked(float lengths_m[OBJECT_STORE_POD_COUNT])
{
    if (lengths_m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    object_store_default_cables(lengths_m);
    if (!storage_manager_path_exists(OBJECT_STORE_SETTINGS_PATH)) {
        return ESP_OK;
    }

    char buffer[1024];
    esp_err_t err = storage_manager_read_text(OBJECT_STORE_SETTINGS_PATH, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "cable_lengths_m");
    if (cJSON_IsArray(array)) {
        for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
            cJSON *item = cJSON_GetArrayItem(array, (int)i);
            if (cJSON_IsNumber(item) && object_store_cable_valid((float)item->valuedouble)) {
                lengths_m[i] = (float)item->valuedouble;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t object_store_settings_write_locked(const float lengths_m[OBJECT_STORE_POD_COUNT])
{
    cJSON *root = cJSON_CreateObject();
    cJSON *array = root == NULL ? NULL : cJSON_AddArrayToObject(root, "cable_lengths_m");
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *item = cJSON_CreateNumber(lengths_m[i]);
        if (item == NULL) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(array, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_manager_atomic_write_text(OBJECT_STORE_SETTINGS_PATH, json);
    cJSON_free(json);
    return err;
}

static void object_store_copy_name(cJSON *root, const char *object_id, char *name, size_t name_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "name");
    const char *source = cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0'
        ? item->valuestring
        : object_id;
    snprintf(name, name_size, "%s", source);
}

esp_err_t object_store_json_read_name_locked(const char *object_id, char *name, size_t name_size)
{
    if (name == NULL || name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_load_metadata_locked(object_id, &root);
    if (err != ESP_OK) {
        return err;
    }

    object_store_copy_name(root, object_id, name, name_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static void object_store_read_location(cJSON *root, object_store_location_t *location)
{
    memset(location, 0, sizeof(*location));
    snprintf(location->source, sizeof(location->source), "NONE");

    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "location");
    if (!cJSON_IsObject(object)) {
        return;
    }

    cJSON *valid = cJSON_GetObjectItemCaseSensitive(object, "valid");
    cJSON *latitude = cJSON_GetObjectItemCaseSensitive(object, "latitude");
    cJSON *longitude = cJSON_GetObjectItemCaseSensitive(object, "longitude");
    cJSON *accuracy = cJSON_GetObjectItemCaseSensitive(object, "accuracy_m");

    location->valid = cJSON_IsTrue(valid) && cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude);
    if (cJSON_IsNumber(latitude)) location->latitude = latitude->valuedouble;
    if (cJSON_IsNumber(longitude)) location->longitude = longitude->valuedouble;
    if (cJSON_IsNumber(accuracy)) location->accuracy_m = (float)accuracy->valuedouble;

    cJSON *altitude_valid = cJSON_GetObjectItemCaseSensitive(object, "altitude_valid");
    cJSON *altitude = cJSON_GetObjectItemCaseSensitive(object, "altitude_m");
    cJSON *altitude_accuracy = cJSON_GetObjectItemCaseSensitive(object, "altitude_accuracy_m");
    location->altitude_valid = cJSON_IsTrue(altitude_valid) && cJSON_IsNumber(altitude);
    if (cJSON_IsNumber(altitude)) location->altitude_m = altitude->valuedouble;
    if (cJSON_IsNumber(altitude_accuracy)) location->altitude_accuracy_m = (float)altitude_accuracy->valuedouble;

    cJSON *captured = cJSON_GetObjectItemCaseSensitive(object, "captured_at_ms");
    if (cJSON_IsNumber(captured) && captured->valuedouble >= 0) {
        location->captured_at_ms = (uint64_t)captured->valuedouble;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(object, "source");
    if (cJSON_IsString(source) && source->valuestring != NULL) {
        snprintf(location->source, sizeof(location->source), "%s", source->valuestring);
    }
}

static void object_store_read_cables(cJSON *root, float lengths[OBJECT_STORE_POD_COUNT])
{
    if (object_store_settings_load_locked(lengths) != ESP_OK) {
        object_store_default_cables(lengths);
    }

    cJSON *pods = cJSON_GetObjectItemCaseSensitive(root, "pods");
    if (!cJSON_IsArray(pods)) {
        return;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *pod = cJSON_GetArrayItem(pods, (int)i);
        cJSON *length = cJSON_IsObject(pod)
            ? cJSON_GetObjectItemCaseSensitive(pod, "cable_length_m")
            : NULL;
        if (cJSON_IsNumber(length) && object_store_cable_valid((float)length->valuedouble)) {
            lengths[i] = (float)length->valuedouble;
        }
    }
}

esp_err_t object_store_json_read_profile_locked(
    const char *object_id,
    bool active,
    object_store_profile_t *profile
)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_load_metadata_locked(object_id, &root);
    if (err != ESP_OK) {
        return err;
    }

    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    object_store_copy_name(root, object_id, profile->summary.name, sizeof(profile->summary.name));
    profile->summary.active = active;
    object_store_read_location(root, &profile->location);
    object_store_read_cables(root, profile->cable_lengths_m);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    if (!object_format_id_is_valid(object_id) || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    if (!object_store_fs_object_exists(object_id)) {
        err = ESP_ERR_NOT_FOUND;
    }
    else {
        err = object_store_json_read_profile_locked(
            object_id,
            strcmp(object_store_internal_active_id(), object_id) == 0,
            profile
        );
    }

    object_store_internal_end_file_operation();
    return err;
}

esp_err_t object_store_list_profiles(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count
)
{
    if (objects == NULL || object_count == NULL || max_objects == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0;
    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    object_store_summary_t summaries[OBJECT_STORE_MAX_OBJECTS];
    size_t count = 0;
    const size_t bounded = max_objects < OBJECT_STORE_MAX_OBJECTS ? max_objects : OBJECT_STORE_MAX_OBJECTS;
    err = object_store_fs_list(summaries, bounded, &count, object_store_internal_active_id());

    if (err == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            err = object_store_json_read_profile_locked(
                summaries[i].id,
                summaries[i].active,
                &objects[i]
            );
            if (err != ESP_OK) break;
        }
    }

    if (err == ESP_OK) *object_count = count;
    object_store_internal_end_file_operation();
    return err;
}

static esp_err_t object_store_mutation_begin(const char *object_id, cJSON **root_out)
{
    if (!object_format_id_is_valid(object_id)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    if (!object_store_fs_object_exists(object_id)) {
        object_store_internal_end_file_operation();
        return ESP_ERR_NOT_FOUND;
    }

    err = object_store_load_metadata_locked(object_id, root_out);
    if (err != ESP_OK) object_store_internal_end_file_operation();
    return err;
}

static esp_err_t object_store_mutation_finish(const char *object_id, cJSON *root)
{
    esp_err_t err = object_store_write_metadata_locked(object_id, root);
    cJSON_Delete(root);
    object_store_internal_end_file_operation();
    return err;
}

esp_err_t object_store_rename(const char *object_id, const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    size_t length = strlen(name);
    if (length == 0 || length >= OBJECT_STORE_NAME_SIZE) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 0x20) return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_mutation_begin(object_id, &root);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObjectCaseSensitive(root, "name");
    if (cJSON_AddStringToObject(root, "name", name) == NULL) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }
    return object_store_mutation_finish(object_id, root);
}

esp_err_t object_store_set_location(
    const char *object_id,
    const object_store_location_t *location
)
{
    if (location == NULL ||
        !location->valid ||
        !isfinite(location->latitude) ||
        !isfinite(location->longitude) ||
        location->latitude < -90.0 || location->latitude > 90.0 ||
        location->longitude < -180.0 || location->longitude > 180.0 ||
        !isfinite(location->accuracy_m) || location->accuracy_m < 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_mutation_begin(object_id, &root);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObjectCaseSensitive(root, "location");
    cJSON *value = cJSON_AddObjectToObject(root, "location");
    bool ok = value != NULL &&
        cJSON_AddBoolToObject(value, "valid", true) != NULL &&
        cJSON_AddNumberToObject(value, "latitude", location->latitude) != NULL &&
        cJSON_AddNumberToObject(value, "longitude", location->longitude) != NULL &&
        cJSON_AddNumberToObject(value, "accuracy_m", location->accuracy_m) != NULL &&
        cJSON_AddBoolToObject(value, "altitude_valid", location->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(value, "altitude_m", location->altitude_valid ? location->altitude_m : 0.0) != NULL &&
        cJSON_AddNumberToObject(value, "altitude_accuracy_m", location->altitude_valid ? location->altitude_accuracy_m : 0.0) != NULL &&
        cJSON_AddNumberToObject(value, "captured_at_ms", (double)location->captured_at_ms) != NULL &&
        cJSON_AddStringToObject(value, "source", location->source[0] ? location->source : "PHONE_GPS") != NULL;

    if (!ok) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }
    return object_store_mutation_finish(object_id, root);
}

static cJSON *object_store_ensure_pods(cJSON *root, const float defaults[OBJECT_STORE_POD_COUNT])
{
    cJSON *pods = cJSON_GetObjectItemCaseSensitive(root, "pods");
    if (cJSON_IsArray(pods) && cJSON_GetArraySize(pods) >= OBJECT_STORE_POD_COUNT) {
        return pods;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(root, "pods");
    pods = cJSON_AddArrayToObject(root, "pods");
    if (pods == NULL) return NULL;

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *pod = cJSON_CreateObject();
        if (pod == NULL ||
            cJSON_AddNumberToObject(pod, "id", i + 1) == NULL ||
            cJSON_AddNumberToObject(pod, "cable_length_m", defaults[i]) == NULL) {
            cJSON_Delete(pod);
            return NULL;
        }
        cJSON_AddItemToArray(pods, pod);
    }
    return pods;
}

esp_err_t object_store_set_cable_length(
    const char *object_id,
    size_t pod_index,
    float length_m
)
{
    if (pod_index >= OBJECT_STORE_POD_COUNT || !object_store_cable_valid(length_m)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_mutation_begin(object_id, &root);
    if (err != ESP_OK) return err;

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    cJSON *pods = object_store_ensure_pods(root, defaults);
    cJSON *pod = pods == NULL ? NULL : cJSON_GetArrayItem(pods, (int)pod_index);
    if (!cJSON_IsObject(pod)) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(pod, "cable_length_m");
    if (cJSON_AddNumberToObject(pod, "cable_length_m", length_m) == NULL) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }
    return object_store_mutation_finish(object_id, root);
}

esp_err_t object_store_get_cable_defaults(float lengths_m[OBJECT_STORE_POD_COUNT])
{
    if (lengths_m == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;
    err = object_store_settings_load_locked(lengths_m);
    object_store_internal_end_file_operation();
    return err;
}

esp_err_t object_store_set_cable_default(size_t pod_index, float length_m)
{
    if (pod_index >= OBJECT_STORE_POD_COUNT || !object_store_cable_valid(length_m)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    float lengths[OBJECT_STORE_POD_COUNT];
    err = object_store_settings_load_locked(lengths);
    if (err == ESP_OK) {
        lengths[pod_index] = length_m;
        err = object_store_settings_write_locked(lengths);
    }

    object_store_internal_end_file_operation();
    return err;
}
