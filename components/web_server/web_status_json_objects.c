#include "web_server_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "object_store.h"

#define WEB_OBJECT_CACHE_TTL_US 30000000LL

static const char *TAG = "WEB_OBJECT_CACHE";

static object_store_profile_t *s_profiles = NULL;
static size_t s_profile_count = 0;
static float s_defaults[OBJECT_STORE_POD_COUNT];
static bool s_cache_valid = false;
static int64_t s_cache_timestamp_us = 0;

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

static esp_err_t web_status_objects_allocate(void)
{
    if (s_profiles != NULL) {
        return ESP_OK;
    }

    s_profiles = heap_caps_calloc(
        OBJECT_STORE_MAX_OBJECTS,
        sizeof(*s_profiles),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (s_profiles == NULL) {
        ESP_LOGW(TAG, "PSRAM profile cache allocation failed; falling back to default heap");
        s_profiles = calloc(OBJECT_STORE_MAX_OBJECTS, sizeof(*s_profiles));
    }

    return s_profiles == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t web_status_objects_refresh_if_needed(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (
        s_cache_valid &&
        now_us - s_cache_timestamp_us >= 0 &&
        now_us - s_cache_timestamp_us < WEB_OBJECT_CACHE_TTL_US
    ) {
        return ESP_OK;
    }

    esp_err_t err = web_status_objects_allocate();
    if (err != ESP_OK) {
        return err;
    }

    const int64_t started_us = now_us;
    size_t count = 0;
    float defaults[OBJECT_STORE_POD_COUNT] = {0};

    err = object_store_list_profiles_with_defaults(
        s_profiles,
        OBJECT_STORE_MAX_OBJECTS,
        &count,
        defaults
    );
    if (err != ESP_OK) {
        s_cache_valid = false;
        ESP_LOGW(TAG, "Cold object refresh failed: %s", esp_err_to_name(err));
        return err;
    }

    s_profile_count = count;
    memcpy(s_defaults, defaults, sizeof(s_defaults));
    s_cache_timestamp_us = esp_timer_get_time();
    s_cache_valid = true;

    ESP_LOGI(
        TAG,
        "Cold object refresh: objects=%u elapsed=%lld ms",
        (unsigned)count,
        (s_cache_timestamp_us - started_us) / 1000LL
    );
    return ESP_OK;
}

void web_status_json_objects_invalidate(void)
{
    s_cache_valid = false;
}

bool web_status_json_add_objects(cJSON *root)
{
    cJSON *store = cJSON_AddObjectToObject(root, "object_store");
    if (store == NULL) return false;

    const esp_err_t err = web_status_objects_refresh_if_needed();

    if (cJSON_AddBoolToObject(store, "available", err == ESP_OK) == NULL) {
        return false;
    }

    if (err != ESP_OK) {
        cJSON_AddNullToObject(store, "active_id");
        cJSON_AddStringToObject(store, "error", esp_err_to_name(err));
        cJSON_AddArrayToObject(store, "default_cable_lengths_m");
        cJSON_AddArrayToObject(store, "objects");
        return true;
    }

    const char *active_id = NULL;
    for (size_t i = 0; i < s_profile_count; ++i) {
        if (s_profiles[i].summary.active) {
            active_id = s_profiles[i].summary.id;
            break;
        }
    }
    if (active_id != NULL) cJSON_AddStringToObject(store, "active_id", active_id);
    else cJSON_AddNullToObject(store, "active_id");

    cJSON *default_array = cJSON_AddArrayToObject(store, "default_cable_lengths_m");
    cJSON *objects = cJSON_AddArrayToObject(store, "objects");
    if (default_array == NULL || objects == NULL) {
        return false;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *number = cJSON_CreateNumber(s_defaults[i]);
        if (number == NULL) {
            return false;
        }
        cJSON_AddItemToArray(default_array, number);
    }

    for (size_t i = 0; i < s_profile_count; ++i) {
        cJSON *object = cJSON_CreateObject();
        if (object == NULL) {
            return false;
        }
        cJSON_AddItemToArray(objects, object);

        if (cJSON_AddStringToObject(object, "id", s_profiles[i].summary.id) == NULL ||
            cJSON_AddStringToObject(object, "name", s_profiles[i].summary.name) == NULL ||
            cJSON_AddBoolToObject(object, "active", s_profiles[i].summary.active) == NULL ||
            !web_status_json_add_location(object, &s_profiles[i].location)) {
            return false;
        }

        cJSON *cables = cJSON_AddArrayToObject(object, "cable_lengths_m");
        if (cables == NULL) {
            return false;
        }
        for (size_t pod = 0; pod < OBJECT_STORE_POD_COUNT; ++pod) {
            cJSON *number = cJSON_CreateNumber(s_profiles[i].cable_lengths_m[pod]);
            if (number == NULL) {
                return false;
            }
            cJSON_AddItemToArray(cables, number);
        }
    }

    return true;
}
