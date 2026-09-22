#include "object_store.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "object_format.h"
#include "object_store_internal.h"
#include "storage_manager.h"

#define OBJECT_STORE_JSON_BUFFER_SIZE 4096
#define OBJECT_STORE_SETTINGS_BUFFER_SIZE 1024U
#define OBJECT_STORE_SETTINGS_PATH STORAGE_MANAGER_ROOT_PATH "/settings.json"
#define OBJECT_STORE_MIN_CABLE_LENGTH_M 0.1f
#define OBJECT_STORE_MAX_CABLE_LENGTH_M 100.0f
#define OBJECT_STORE_INDEX_RECONCILE_INTERVAL_US 60000000LL

static const char *TAG = "OBJECT_STORE_INDEX";

/*
 * The SD card is authoritative, but serving /api/status must not re-open every
 * object.json. The cache is protected by the object-store mutex. At 32
 * profiles it is only a few KiB and avoids a multi-second FAT traversal.
 */
static object_store_profile_t s_profile_index[OBJECT_STORE_MAX_OBJECTS];
static size_t s_profile_index_count = 0;
static bool s_profile_index_valid = false;
static int64_t s_profile_index_reconcile_us = 0;

/* settings.json is tiny and changes only through object_store_set_cable_default. */
static float s_settings_cache[OBJECT_STORE_POD_COUNT];
static bool s_settings_cache_valid = false;

static void object_store_profile_index_invalidate_membership_locked(void)
{
    s_profile_index_count = 0;
    s_profile_index_valid = false;
    s_profile_index_reconcile_us = 0;
    memset(s_profile_index, 0, sizeof(s_profile_index));
}

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

esp_err_t object_store_internal_validate_name(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;

    const size_t length = strlen(name);
    if (length == 0 || length >= OBJECT_STORE_NAME_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)name[i] < 0x20) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static int object_store_profile_index_find_locked(const char *object_id)
{
    for (size_t i = 0; i < s_profile_index_count; ++i) {
        if (strcmp(s_profile_index[i].summary.id, object_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void object_store_profile_index_sort_locked(void)
{
    for (size_t i = 0; i < s_profile_index_count; ++i) {
        for (size_t j = i + 1; j < s_profile_index_count; ++j) {
            if (strcmp(s_profile_index[i].summary.id, s_profile_index[j].summary.id) <= 0) {
                continue;
            }
            object_store_profile_t temp = s_profile_index[i];
            s_profile_index[i] = s_profile_index[j];
            s_profile_index[j] = temp;
        }
    }
}

static void object_store_profile_index_apply_active_locked(void)
{
    const char *active_id = object_store_internal_active_id();
    for (size_t i = 0; i < s_profile_index_count; ++i) {
        s_profile_index[i].summary.active =
            active_id[0] != '\0' && strcmp(s_profile_index[i].summary.id, active_id) == 0;
    }
}

void object_store_profile_cache_reset(void)
{
    object_store_profile_index_invalidate_membership_locked();
    s_settings_cache_valid = false;
    memset(s_settings_cache, 0, sizeof(s_settings_cache));
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

    char *buffer = malloc(OBJECT_STORE_JSON_BUFFER_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = storage_manager_read_text(path, buffer, OBJECT_STORE_JSON_BUFFER_SIZE);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(
        root,
        "schema_version"
    );
    const cJSON *embedded_id = cJSON_GetObjectItemCaseSensitive(
        root,
        "id"
    );

    if (
        !cJSON_IsNumber(schema_version) ||
        !cJSON_IsString(embedded_id) ||
        embedded_id->valuestring == NULL ||
        !object_format_metadata_identity_is_valid(
            schema_version->valueint,
            embedded_id->valuestring,
            object_id
        )
    ) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *root_out = root;
    return ESP_OK;
}

static bool object_store_profile_error_is_omittable(esp_err_t err)
{
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE;
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

    if (s_settings_cache_valid) {
        memcpy(lengths_m, s_settings_cache, sizeof(s_settings_cache));
        return ESP_OK;
    }

    float loaded[OBJECT_STORE_POD_COUNT];
    object_store_default_cables(loaded);

    /*
     * A missing settings.json means defaults, not an error. Cache that fact so
     * we do not spend ~100 ms on the same negative FAT stat for every profile
     * read and mutation.
     */
    if (!storage_manager_path_exists(OBJECT_STORE_SETTINGS_PATH)) {
        memcpy(s_settings_cache, loaded, sizeof(s_settings_cache));
        s_settings_cache_valid = true;
        memcpy(lengths_m, loaded, sizeof(loaded));
        return ESP_OK;
    }

    char *buffer = malloc(OBJECT_STORE_SETTINGS_BUFFER_SIZE);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = storage_manager_read_text(
        OBJECT_STORE_SETTINGS_PATH,
        buffer,
        OBJECT_STORE_SETTINGS_BUFFER_SIZE
    );
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "cable_lengths_m");
    if (cJSON_IsArray(array)) {
        for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
            cJSON *item = cJSON_GetArrayItem(array, (int)i);
            if (cJSON_IsNumber(item) && object_store_cable_valid((float)item->valuedouble)) {
                loaded[i] = (float)item->valuedouble;
            }
        }
    }

    cJSON_Delete(root);
    memcpy(s_settings_cache, loaded, sizeof(s_settings_cache));
    s_settings_cache_valid = true;
    memcpy(lengths_m, loaded, sizeof(loaded));
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
    if (err == ESP_OK) {
        memcpy(s_settings_cache, lengths_m, sizeof(s_settings_cache));
        s_settings_cache_valid = true;
    }
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

static void object_store_read_cables(
    cJSON *root,
    const float defaults[OBJECT_STORE_POD_COUNT],
    float lengths[OBJECT_STORE_POD_COUNT]
)
{
    memcpy(lengths, defaults, sizeof(float) * OBJECT_STORE_POD_COUNT);

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

static void object_store_read_geometry(
    cJSON *root,
    object_store_geometry_t *geometry
)
{
    /* geometry defaults to zero/not-computed via the caller's memset;
     * absent or malformed JSON leaves it exactly there -- old objects
     * written before MAX-118 have no "geometry" key at all. */
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "geometry");
    if (!cJSON_IsObject(node)) {
        return;
    }

    cJSON *anchored = cJSON_GetObjectItemCaseSensitive(node, "anchored");
    geometry->anchored = cJSON_IsTrue(anchored);

    cJSON *pods = cJSON_GetObjectItemCaseSensitive(node, "pods");
    if (!cJSON_IsArray(pods)) {
        return;
    }

    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        cJSON *pod = cJSON_GetArrayItem(pods, (int)i);
        if (!cJSON_IsObject(pod)) {
            continue;
        }
        cJSON *computed = cJSON_GetObjectItemCaseSensitive(pod, "computed");
        if (!cJSON_IsTrue(computed)) {
            continue;
        }
        cJSON *leveled = cJSON_GetObjectItemCaseSensitive(pod, "leveled");
        cJSON *x_mm = cJSON_GetObjectItemCaseSensitive(pod, "x_mm");
        cJSON *y_mm = cJSON_GetObjectItemCaseSensitive(pod, "y_mm");
        cJSON *z_mm = cJSON_GetObjectItemCaseSensitive(pod, "z_mm");
        cJSON *computed_at = cJSON_GetObjectItemCaseSensitive(pod, "computed_at_ms");
        if (!cJSON_IsNumber(x_mm) || !cJSON_IsNumber(y_mm) || !cJSON_IsNumber(z_mm)) {
            continue;
        }
        geometry->pods[i].computed = true;
        geometry->pods[i].leveled = cJSON_IsTrue(leveled);
        geometry->pods[i].x_mm = (int32_t)x_mm->valuedouble;
        geometry->pods[i].y_mm = (int32_t)y_mm->valuedouble;
        geometry->pods[i].z_mm = (int32_t)z_mm->valuedouble;
        geometry->pods[i].computed_at_ms = cJSON_IsNumber(computed_at) && computed_at->valuedouble >= 0
            ? (uint64_t)computed_at->valuedouble
            : 0U;
    }
}

static void object_store_read_cs_pairs(
    cJSON *root,
    object_store_cs_pairs_t *cs_pairs
)
{
    /* cs_pairs defaults to zero/empty via the caller's memset; absent or
     * malformed JSON leaves it exactly there -- objects written before
     * MAX-148 have no "cs_pairs" key at all. */
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, "cs_pairs");
    if (!cJSON_IsObject(node)) {
        return;
    }

    cJSON *pairs = cJSON_GetObjectItemCaseSensitive(node, "pairs");
    if (!cJSON_IsArray(pairs)) {
        return;
    }

    size_t count = 0U;
    const int array_size = cJSON_GetArraySize(pairs);
    for (int i = 0; i < array_size && count < OBJECT_STORE_MAX_CS_PAIRS; ++i) {
        cJSON *pair = cJSON_GetArrayItem(pairs, i);
        if (!cJSON_IsObject(pair)) {
            continue;
        }
        cJSON *node_a = cJSON_GetObjectItemCaseSensitive(pair, "node_a");
        cJSON *node_b = cJSON_GetObjectItemCaseSensitive(pair, "node_b");
        cJSON *valid = cJSON_GetObjectItemCaseSensitive(pair, "valid");
        cJSON *distance_mm = cJSON_GetObjectItemCaseSensitive(pair, "distance_mm");
        cJSON *confidence = cJSON_GetObjectItemCaseSensitive(pair, "confidence");
        cJSON *attempts = cJSON_GetObjectItemCaseSensitive(pair, "attempts");
        cJSON *valid_samples = cJSON_GetObjectItemCaseSensitive(pair, "valid_samples");
        if (
            !cJSON_IsNumber(node_a) || !cJSON_IsNumber(node_b) ||
            !cJSON_IsNumber(distance_mm) || !cJSON_IsNumber(confidence) ||
            !cJSON_IsNumber(attempts) || !cJSON_IsNumber(valid_samples)
        ) {
            continue;
        }
        object_store_cs_pair_t *out = &cs_pairs->pairs[count];
        out->node_a = (uint8_t)node_a->valuedouble;
        out->node_b = (uint8_t)node_b->valuedouble;
        out->valid = cJSON_IsTrue(valid);
        out->distance_mm = (int32_t)distance_mm->valuedouble;
        out->confidence = (uint16_t)confidence->valuedouble;
        out->attempts = (uint16_t)attempts->valuedouble;
        out->valid_samples = (uint8_t)valid_samples->valuedouble;
        ++count;
    }
    cs_pairs->pair_count = count;
}


static void object_store_profile_from_root_locked(
    const char *object_id,
    bool active,
    const float defaults[OBJECT_STORE_POD_COUNT],
    cJSON *root,
    object_store_profile_t *profile
)
{
    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    object_store_copy_name(root, object_id, profile->summary.name, sizeof(profile->summary.name));
    profile->summary.active = active;
    object_store_read_location(root, &profile->location);
    object_store_read_cables(root, defaults, profile->cable_lengths_m);
    object_store_read_geometry(root, &profile->geometry);
    object_store_read_cs_pairs(root, &profile->cs_pairs);
}

static esp_err_t object_store_json_read_profile_with_defaults_locked(
    const char *object_id,
    bool active,
    const float defaults[OBJECT_STORE_POD_COUNT],
    object_store_profile_t *profile
)
{
    if (profile == NULL || defaults == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_load_metadata_locked(object_id, &root);
    if (err != ESP_OK) {
        return err;
    }

    object_store_profile_from_root_locked(object_id, active, defaults, root, profile);
    cJSON_Delete(root);
    return ESP_OK;
}

static void object_store_profile_index_upsert_from_root_locked(const char *object_id, cJSON *root)
{
    if (!s_profile_index_valid || root == NULL) return;

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    object_store_profile_t profile;
    object_store_profile_from_root_locked(
        object_id,
        strcmp(object_store_internal_active_id(), object_id) == 0,
        defaults,
        root,
        &profile
    );

    int found = object_store_profile_index_find_locked(object_id);
    if (found >= 0) {
        s_profile_index[(size_t)found] = profile;
    }
    else if (s_profile_index_count < OBJECT_STORE_MAX_OBJECTS) {
        s_profile_index[s_profile_index_count++] = profile;
        object_store_profile_index_sort_locked();
    }
}

esp_err_t object_store_json_read_profile_locked(
    const char *object_id,
    bool active,
    object_store_profile_t *profile
)
{
    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    return object_store_json_read_profile_with_defaults_locked(object_id, active, defaults, profile);
}

static esp_err_t object_store_profile_index_build_locked(void)
{
    const int64_t started_us = esp_timer_get_time();
    /*
     * This census is non-realtime storage work.  Keeping 32 summaries on the
     * caller's stack made index rebuild/reconcile consume roughly 2.7 KiB
     * before their FATFS/cJSON callees.  Heap ownership is local to this
     * operation and is released on every return path.
     */
    object_store_summary_t *summaries = calloc(
        OBJECT_STORE_MAX_OBJECTS,
        sizeof(*summaries)
    );
    if (summaries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;

    esp_err_t err = object_store_fs_list(
        summaries,
        OBJECT_STORE_MAX_OBJECTS,
        &count,
        object_store_internal_active_id()
    );
    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    s_profile_index_count = 0;
    for (size_t i = 0; i < count; ++i) {
        object_store_profile_t profile;
        err = object_store_json_read_profile_with_defaults_locked(
            summaries[i].id,
            summaries[i].active,
            defaults,
            &profile
        );
        if (object_store_profile_error_is_omittable(err)) {
            ESP_LOGW(
                TAG,
                "Skipping invalid/disappeared object during index build: %s err=%s",
                summaries[i].id,
                esp_err_to_name(err)
            );
            continue;
        }
        if (err != ESP_OK) {
            s_profile_index_count = 0;
            s_profile_index_valid = false;
            ESP_LOGW(TAG, "Index build failed at %s: %s", summaries[i].id, esp_err_to_name(err));
            free(summaries);
            return err;
        }
        s_profile_index[s_profile_index_count++] = profile;
    }

    s_profile_index_valid = true;
    s_profile_index_reconcile_us = esp_timer_get_time();
    ESP_LOGI(
        TAG,
        "Index built: objects=%u elapsed=%lld ms",
        (unsigned)s_profile_index_count,
        (s_profile_index_reconcile_us - started_us) / 1000LL
    );
    free(summaries);
    return ESP_OK;
}

static bool object_store_summary_contains_id(
    const object_store_summary_t *summaries,
    size_t count,
    const char *object_id
)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(summaries[i].id, object_id) == 0) return true;
    }
    return false;
}

static esp_err_t object_store_profile_index_reconcile_locked(void)
{
    const int64_t started_us = esp_timer_get_time();
    /* The directory census is bounded but too large for a service call stack. */
    object_store_summary_t *summaries = calloc(
        OBJECT_STORE_MAX_OBJECTS,
        sizeof(*summaries)
    );
    if (summaries == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    esp_err_t err = object_store_fs_list(
        summaries,
        OBJECT_STORE_MAX_OBJECTS,
        &count,
        object_store_internal_active_id()
    );
    if (err != ESP_OK) {
        free(summaries);
        return err;
    }

    size_t removed = 0;
    for (size_t i = 0; i < s_profile_index_count;) {
        if (object_store_summary_contains_id(summaries, count, s_profile_index[i].summary.id)) {
            i++;
            continue;
        }
        if (i + 1 < s_profile_index_count) {
            memmove(
                &s_profile_index[i],
                &s_profile_index[i + 1],
                (s_profile_index_count - i - 1) * sizeof(s_profile_index[0])
            );
        }
        s_profile_index_count--;
        removed++;
    }

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) {
        object_store_default_cables(defaults);
    }

    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        int found = object_store_profile_index_find_locked(summaries[i].id);
        if (found >= 0) {
            s_profile_index[(size_t)found].summary.active = summaries[i].active;
            continue;
        }
        if (s_profile_index_count >= OBJECT_STORE_MAX_OBJECTS) break;

        object_store_profile_t profile;
        err = object_store_json_read_profile_with_defaults_locked(
            summaries[i].id,
            summaries[i].active,
            defaults,
            &profile
        );
        if (object_store_profile_error_is_omittable(err)) {
            ESP_LOGW(
                TAG,
                "Skipping invalid/disappeared object during reconcile: %s err=%s",
                summaries[i].id,
                esp_err_to_name(err)
            );
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Reconcile failed for new %s: %s", summaries[i].id, esp_err_to_name(err));
            free(summaries);
            return err;
        }
        s_profile_index[s_profile_index_count++] = profile;
        added++;
    }

    object_store_profile_index_apply_active_locked();
    object_store_profile_index_sort_locked();
    s_profile_index_reconcile_us = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "Index reconcile: census=%u cached=%u added=%u removed=%u elapsed=%lld ms",
        (unsigned)count,
        (unsigned)s_profile_index_count,
        (unsigned)added,
        (unsigned)removed,
        (s_profile_index_reconcile_us - started_us) / 1000LL
    );
    free(summaries);
    return ESP_OK;
}

static void object_store_profile_index_copy_locked(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    const size_t count = s_profile_index_count < max_objects ? s_profile_index_count : max_objects;
    object_store_profile_index_apply_active_locked();
    memcpy(objects, s_profile_index, count * sizeof(objects[0]));
    *object_count = count;

    if (s_settings_cache_valid) {
        memcpy(default_cable_lengths_m, s_settings_cache, sizeof(s_settings_cache));
    }
    else {
        object_store_default_cables(default_cable_lengths_m);
    }
}

esp_err_t object_store_get_profile(const char *object_id, object_store_profile_t *profile)
{
    if (!object_format_id_is_valid(object_id) || profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Hot path: no SD/LVGL arbitration at all when the index already knows it. */
    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) return err;
    if (s_profile_index_valid) {
        int found = object_store_profile_index_find_locked(object_id);
        if (found >= 0) {
            *profile = s_profile_index[(size_t)found];
            profile->summary.active = strcmp(object_store_internal_active_id(), object_id) == 0;
            object_store_internal_end_state_operation();
            return ESP_OK;
        }
    }
    object_store_internal_end_state_operation();

    err = object_store_internal_begin_file_operation();
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
        if (err == ESP_OK && s_profile_index_valid) {
            int found = object_store_profile_index_find_locked(object_id);
            if (found >= 0) s_profile_index[(size_t)found] = *profile;
            else if (s_profile_index_count < OBJECT_STORE_MAX_OBJECTS) {
                s_profile_index[s_profile_index_count++] = *profile;
                object_store_profile_index_sort_locked();
            }
        }
    }

    object_store_internal_end_file_operation();
    return err;
}

esp_err_t object_store_list_profiles_with_defaults(
    object_store_profile_t *objects,
    size_t max_objects,
    size_t *object_count,
    float default_cable_lengths_m[OBJECT_STORE_POD_COUNT]
)
{
    if (
        objects == NULL || object_count == NULL ||
        default_cable_lengths_m == NULL || max_objects == 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    *object_count = 0;
    const int64_t now_us = esp_timer_get_time();

    /* Pure RAM snapshot while the periodic directory census is still fresh. */
    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) return err;
    if (
        s_profile_index_valid &&
        now_us - s_profile_index_reconcile_us >= 0 &&
        now_us - s_profile_index_reconcile_us < OBJECT_STORE_INDEX_RECONCILE_INTERVAL_US
    ) {
        object_store_profile_index_copy_locked(
            objects,
            max_objects,
            object_count,
            default_cable_lengths_m
        );
        object_store_internal_end_state_operation();
        return ESP_OK;
    }
    object_store_internal_end_state_operation();

    /* Cold build, or a cheap topology-only reconciliation once per minute. */
    err = object_store_internal_begin_file_operation();
    if (err != ESP_OK) return err;

    const int64_t locked_now_us = esp_timer_get_time();
    if (!s_profile_index_valid) {
        err = object_store_profile_index_build_locked();
    }
    else if (
        locked_now_us - s_profile_index_reconcile_us >= OBJECT_STORE_INDEX_RECONCILE_INTERVAL_US ||
        locked_now_us < s_profile_index_reconcile_us
    ) {
        err = object_store_profile_index_reconcile_locked();
    }

    if (err == ESP_OK) {
        object_store_profile_index_copy_locked(
            objects,
            max_objects,
            object_count,
            default_cable_lengths_m
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
    float defaults[OBJECT_STORE_POD_COUNT];
    return object_store_list_profiles_with_defaults(objects, max_objects, object_count, defaults);
}

void object_store_profile_cache_note_create_locked(
    const char *object_id,
    const char *name
)
{
    if (!s_profile_index_valid || !object_format_id_is_valid(object_id)) return;

    const int found = object_store_profile_index_find_locked(object_id);
    if (object_store_profile_cache_create_requires_membership_refresh(
        s_profile_index_valid,
        s_profile_index_count,
        found >= 0
    )) {
        object_store_profile_index_invalidate_membership_locked();
        return;
    }

    if (found >= 0) {
        object_store_profile_cache_note_rename_locked(
            object_id,
            name != NULL ? name : object_id
        );
        object_store_profile_cache_note_active_locked(object_id);
        return;
    }

    object_store_profile_t *profile = &s_profile_index[s_profile_index_count++];
    memset(profile, 0, sizeof(*profile));
    snprintf(profile->summary.id, sizeof(profile->summary.id), "%s", object_id);
    snprintf(
        profile->summary.name,
        sizeof(profile->summary.name),
        "%s",
        name != NULL ? name : object_id
    );
    snprintf(profile->location.source, sizeof(profile->location.source), "NONE");

    float defaults[OBJECT_STORE_POD_COUNT];
    if (object_store_settings_load_locked(defaults) != ESP_OK) object_store_default_cables(defaults);
    memcpy(profile->cable_lengths_m, defaults, sizeof(profile->cable_lengths_m));

    object_store_profile_cache_note_active_locked(object_id);
    object_store_profile_index_sort_locked();
}

void object_store_profile_cache_note_delete_locked(const char *object_id)
{
    if (!s_profile_index_valid) return;

    const int found = object_store_profile_index_find_locked(object_id);
    if (object_store_profile_cache_delete_requires_membership_refresh(
        s_profile_index_valid,
        s_profile_index_count,
        found >= 0
    )) {
        object_store_profile_index_invalidate_membership_locked();
        return;
    }
    if (found < 0) return;

    const size_t index = (size_t)found;
    if (index + 1 < s_profile_index_count) {
        memmove(
            &s_profile_index[index],
            &s_profile_index[index + 1],
            (s_profile_index_count - index - 1) * sizeof(s_profile_index[0])
        );
    }
    s_profile_index_count--;
}

void object_store_profile_cache_note_active_locked(const char *object_id)
{
    if (!s_profile_index_valid) return;

    const int found = object_id != NULL && object_id[0] != '\0'
        ? object_store_profile_index_find_locked(object_id)
        : -1;
    if (object_store_profile_cache_active_requires_membership_refresh(
        s_profile_index_valid,
        found >= 0,
        object_id
    )) {
        object_store_profile_index_invalidate_membership_locked();
        return;
    }

    for (size_t i = 0; i < s_profile_index_count; ++i) {
        s_profile_index[i].summary.active =
            object_id != NULL && object_id[0] != '\0' &&
            strcmp(s_profile_index[i].summary.id, object_id) == 0;
    }
}

void object_store_profile_cache_note_rename_locked(const char *object_id, const char *name)
{
    if (!s_profile_index_valid || name == NULL) return;
    int found = object_store_profile_index_find_locked(object_id);
    if (found >= 0) snprintf(s_profile_index[(size_t)found].summary.name, OBJECT_STORE_NAME_SIZE, "%s", name);
}

void object_store_profile_cache_note_location_locked(
    const char *object_id,
    const object_store_location_t *location
)
{
    if (!s_profile_index_valid || location == NULL) return;
    int found = object_store_profile_index_find_locked(object_id);
    if (found >= 0) s_profile_index[(size_t)found].location = *location;
}

void object_store_profile_cache_note_cable_locked(const char *object_id, size_t pod_index, float length_m)
{
    if (!s_profile_index_valid || pod_index >= OBJECT_STORE_POD_COUNT) return;
    int found = object_store_profile_index_find_locked(object_id);
    if (found >= 0) s_profile_index[(size_t)found].cable_lengths_m[pod_index] = length_m;
}

void object_store_profile_cache_note_default_locked(size_t pod_index, float length_m)
{
    if (pod_index >= OBJECT_STORE_POD_COUNT) return;
    if (!s_settings_cache_valid) object_store_default_cables(s_settings_cache);
    s_settings_cache[pod_index] = length_m;
    s_settings_cache_valid = true;
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
    if (err == ESP_OK) {
        /* Project the already-parsed committed metadata into RAM; no reread. */
        object_store_profile_index_upsert_from_root_locked(object_id, root);
    }
    cJSON_Delete(root);
    object_store_internal_end_file_operation();
    return err;
}

esp_err_t object_store_rename(const char *object_id, const char *name)
{
    esp_err_t err = object_store_internal_validate_name(name);
    if (err != ESP_OK) return err;

    cJSON *root = NULL;
    err = object_store_mutation_begin(object_id, &root);
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

esp_err_t object_store_set_geometry(
    const char *object_id,
    const object_store_geometry_t *geometry
)
{
    static const object_store_geometry_t cleared = {0};
    if (geometry == NULL) {
        geometry = &cleared;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_mutation_begin(object_id, &root);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObjectCaseSensitive(root, "geometry");
    cJSON *node = cJSON_AddObjectToObject(root, "geometry");
    cJSON *pods = node == NULL ? NULL : cJSON_AddArrayToObject(node, "pods");
    bool ok = node != NULL && pods != NULL &&
        cJSON_AddBoolToObject(node, "anchored", geometry->anchored) != NULL;

    for (size_t i = 0; ok && i < OBJECT_STORE_POD_COUNT; ++i) {
        const object_store_pod_geometry_t *pod_geometry = &geometry->pods[i];
        cJSON *pod = cJSON_CreateObject();
        ok = pod != NULL &&
            cJSON_AddNumberToObject(pod, "id", (double)(i + 1)) != NULL &&
            cJSON_AddBoolToObject(pod, "computed", pod_geometry->computed) != NULL &&
            cJSON_AddBoolToObject(pod, "leveled", pod_geometry->leveled) != NULL &&
            cJSON_AddNumberToObject(pod, "x_mm", (double)pod_geometry->x_mm) != NULL &&
            cJSON_AddNumberToObject(pod, "y_mm", (double)pod_geometry->y_mm) != NULL &&
            cJSON_AddNumberToObject(pod, "z_mm", (double)pod_geometry->z_mm) != NULL &&
            cJSON_AddNumberToObject(pod, "computed_at_ms", (double)pod_geometry->computed_at_ms) != NULL;
        if (ok) {
            cJSON_AddItemToArray(pods, pod);
        } else {
            cJSON_Delete(pod);
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }
    return object_store_mutation_finish(object_id, root);
}

esp_err_t object_store_set_cs_pairs(
    const char *object_id,
    const object_store_cs_pairs_t *cs_pairs
)
{
    static const object_store_cs_pairs_t cleared = {0};
    if (cs_pairs == NULL) {
        cs_pairs = &cleared;
    }

    cJSON *root = NULL;
    esp_err_t err = object_store_mutation_begin(object_id, &root);
    if (err != ESP_OK) return err;

    cJSON_DeleteItemFromObjectCaseSensitive(root, "cs_pairs");
    cJSON *node = cJSON_AddObjectToObject(root, "cs_pairs");
    cJSON *pairs = node == NULL ? NULL : cJSON_AddArrayToObject(node, "pairs");
    bool ok = node != NULL && pairs != NULL;

    const size_t pair_count = cs_pairs->pair_count < OBJECT_STORE_MAX_CS_PAIRS
        ? cs_pairs->pair_count
        : OBJECT_STORE_MAX_CS_PAIRS;
    for (size_t i = 0; ok && i < pair_count; ++i) {
        const object_store_cs_pair_t *pair_result = &cs_pairs->pairs[i];
        cJSON *pair = cJSON_CreateObject();
        ok = pair != NULL &&
            cJSON_AddNumberToObject(pair, "node_a", pair_result->node_a) != NULL &&
            cJSON_AddNumberToObject(pair, "node_b", pair_result->node_b) != NULL &&
            cJSON_AddBoolToObject(pair, "valid", pair_result->valid) != NULL &&
            cJSON_AddNumberToObject(pair, "distance_mm", (double)pair_result->distance_mm) != NULL &&
            cJSON_AddNumberToObject(pair, "confidence", pair_result->confidence) != NULL &&
            cJSON_AddNumberToObject(pair, "attempts", pair_result->attempts) != NULL &&
            cJSON_AddNumberToObject(pair, "valid_samples", pair_result->valid_samples) != NULL;
        if (ok) {
            cJSON_AddItemToArray(pairs, pair);
        } else {
            cJSON_Delete(pair);
        }
    }

    if (!ok) {
        cJSON_Delete(root);
        object_store_internal_end_file_operation();
        return ESP_ERR_NO_MEM;
    }
    return object_store_mutation_finish(object_id, root);
}

esp_err_t object_store_get_cable_defaults(float lengths_m[OBJECT_STORE_POD_COUNT])
{
    if (lengths_m == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = object_store_internal_begin_state_operation();
    if (err != ESP_OK) return err;
    if (s_settings_cache_valid) {
        memcpy(lengths_m, s_settings_cache, sizeof(s_settings_cache));
        object_store_internal_end_state_operation();
        return ESP_OK;
    }
    object_store_internal_end_state_operation();

    err = object_store_internal_begin_file_operation();
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
        if (err == ESP_OK) object_store_profile_cache_note_default_locked(pod_index, length_m);
    }

    object_store_internal_end_file_operation();
    return err;
}
