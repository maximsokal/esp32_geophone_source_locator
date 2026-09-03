#include "web_server_internal.h"

#include <stdlib.h>

#include "object_store.h"

static bool web_status_json_add_location(
    cJSON *parent,
    const object_store_location_t *location
)
{
    cJSON *object = cJSON_AddObjectToObject(parent, "location");
    if (object == NULL) return false;

    return
        cJSON_AddBoolToObject(object, "valid", location->valid) != NULL &&
        cJSON_AddNumberToObject(object, "latitude", location->latitude) != NULL &&
        cJSON_AddNumberToObject(object, "longitude", location->longitude) != NULL &&
        cJSON_AddNumberToObject(object, "accuracy_m", location->accuracy_m) != NULL &&
        cJSON_AddBoolToObject(object, "altitude_valid", location->altitude_valid) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_m", location->altitude_m) != NULL &&
        cJSON_AddNumberToObject(object, "altitude_accuracy_m", location->altitude_accuracy_m) != NULL &&
        cJSON_AddNumberToObject(object, "captured_at_ms", (double)location->captured_at_ms) != NULL &&
        cJSON_AddStringToObject(object, "source", location->source) != NULL;
}

bool web_status_json_add_objects(cJSON *root)
{
    cJSON *store = cJSON_AddObjectToObject(root, "object_store");
    if (store == NULL) return false;

    object_store_profile_t *profiles = calloc(
        OBJECT_STORE_MAX_OBJECTS,
        sizeof(*profiles)
    );
    if (profiles == NULL) return false;

    size_t count = 0;
    float defaults[OBJECT_STORE_POD_COUNT];
    esp_err_t err = object_store_list_profiles_with_defaults(
        profiles,
        OBJECT_STORE_MAX_OBJECTS,
        &count,
        defaults
    );

    if (cJSON_AddBoolToObject(store, "available", err == ESP_OK) == NULL) {
        free(profiles);
        return false;
    }

    if (err != ESP_OK) {
        cJSON_AddNullToObject(store, "active_id");
        cJSON_AddStringToObject(store, "error", esp_err_to_name(err));
        cJSON_AddArrayToObject(store, "default_cable_lengths_m");
        cJSON_AddArrayToObject(store, "objects");
        free(profiles);
        return true;
    }

    const char *active_id = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (profiles[i].summary.active) {
            active_id = profiles[i].summary.id;
            break;
        }
    }
    if (active_id != NULL) cJSON_AddStringToObject(store, "active_id", active_id);
    else cJSON_AddNullToObject(store, "active_id");

    cJSON *default_array = cJSON_AddArrayToObject(store, "default_cable_lengths_m");
    cJSON *objects = cJSON_AddArrayToObject(store, "objects");
    if (default_array == NULL || objects == NULL) {
        free(profiles);
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *number = cJSON_CreateNumber(defaults[i]);
        if (number == NULL) {
            free(profiles);
            return false;
        }
        cJSON_AddItemToArray(default_array, number);
    }

    for (size_t i = 0; i < count; ++i) {
        cJSON *object = cJSON_CreateObject();
        if (object == NULL) {
            free(profiles);
            return false;
        }
        cJSON_AddItemToArray(objects, object);

        if (cJSON_AddStringToObject(object, "id", profiles[i].summary.id) == NULL ||
            cJSON_AddStringToObject(object, "name", profiles[i].summary.name) == NULL ||
            cJSON_AddBoolToObject(object, "active", profiles[i].summary.active) == NULL ||
            !web_status_json_add_location(object, &profiles[i].location)) {
            free(profiles);
            return false;
        }

        cJSON *cables = cJSON_AddArrayToObject(object, "cable_lengths_m");
        if (cables == NULL) {
            free(profiles);
            return false;
        }
        for (size_t pod = 0; pod < OBJECT_STORE_POD_COUNT; ++pod) {
            cJSON *number = cJSON_CreateNumber(profiles[i].cable_lengths_m[pod]);
            if (number == NULL) {
                free(profiles);
                return false;
            }
            cJSON_AddItemToArray(cables, number);
        }
    }

    free(profiles);
    return true;
}
