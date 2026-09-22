#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "object_store_internal.h"
#include "storage_manager.h"

static char persisted_json[8192];
static int64_t fake_time_us;

bool storage_manager_path_exists(const char *path)
{
    return path != NULL && strstr(path, "/object.json") != NULL;
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) return ESP_ERR_INVALID_ARG;
    if (!storage_manager_path_exists(path)) return ESP_ERR_NOT_FOUND;
    if (strlen(persisted_json) + 1U > buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, persisted_json, strlen(persisted_json) + 1U);
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) return ESP_ERR_INVALID_ARG;
    if (!storage_manager_path_exists(path)) return ESP_ERR_NOT_FOUND;
    if (strlen(text) + 1U > sizeof(persisted_json)) return ESP_ERR_INVALID_SIZE;
    memcpy(persisted_json, text, strlen(text) + 1U);
    return ESP_OK;
}

int64_t esp_timer_get_time(void)
{
    fake_time_us += 1000;
    return fake_time_us;
}

esp_err_t object_store_internal_begin_file_operation(void)
{
    return ESP_OK;
}

void object_store_internal_end_file_operation(void)
{
}

esp_err_t object_store_internal_begin_state_operation(void)
{
    return ESP_OK;
}

void object_store_internal_end_state_operation(void)
{
}

const char *object_store_internal_active_id(void)
{
    return "";
}

esp_err_t object_store_fs_metadata_path(const char *object_id, char *path, size_t path_size)
{
    if (!object_format_id_is_valid(object_id) || path == NULL || path_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(
        path,
        path_size,
        STORAGE_MANAGER_OBJECTS_PATH "/%s/object.json",
        object_id
    );
    return written < 0 || (size_t)written >= path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

bool object_store_fs_object_exists(const char *object_id)
{
    return object_format_id_is_valid(object_id);
}

esp_err_t object_store_fs_list(
    object_store_summary_t *objects,
    size_t max_objects,
    size_t *object_count,
    const char *active_object_id
)
{
    (void)objects;
    (void)max_objects;
    (void)object_count;
    (void)active_object_id;
    return ESP_ERR_NOT_FOUND;
}

bool object_store_profile_cache_create_requires_membership_refresh(
    bool cache_valid,
    size_t cache_count,
    bool object_present
)
{
    (void)cache_valid;
    (void)cache_count;
    (void)object_present;
    return false;
}

bool object_store_profile_cache_active_requires_membership_refresh(
    bool cache_valid,
    bool target_present,
    const char *object_id
)
{
    (void)cache_valid;
    (void)target_present;
    (void)object_id;
    return false;
}

bool object_store_profile_cache_delete_requires_membership_refresh(
    bool cache_valid,
    size_t cache_count,
    bool object_present
)
{
    (void)cache_valid;
    (void)cache_count;
    (void)object_present;
    return false;
}

#include "../object_store_profile.c"

static void set_metadata(const char *json)
{
    assert(strlen(json) + 1U < sizeof(persisted_json));
    memcpy(persisted_json, json, strlen(json) + 1U);
}

static void assert_cs_pairs_equal(
    const object_store_cs_pairs_t *expected,
    const object_store_cs_pairs_t *actual
)
{
    assert(expected->pair_count == actual->pair_count);
    for (size_t i = 0; i < expected->pair_count; ++i) {
        assert(expected->pairs[i].node_a == actual->pairs[i].node_a);
        assert(expected->pairs[i].node_b == actual->pairs[i].node_b);
        assert(expected->pairs[i].valid == actual->pairs[i].valid);
        assert(expected->pairs[i].distance_mm == actual->pairs[i].distance_mm);
        assert(expected->pairs[i].confidence == actual->pairs[i].confidence);
        assert(expected->pairs[i].attempts == actual->pairs[i].attempts);
        assert(expected->pairs[i].valid_samples == actual->pairs[i].valid_samples);
    }
}

static void test_cs_pairs_write_close_reopen_round_trip(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_profile_t before;
    assert(object_store_get_profile("OBJ_0001", &before) == ESP_OK);
    assert(before.cs_pairs.pair_count == 0U);

    object_store_cs_pairs_t expected = {0};
    expected.pair_count = 2U;
    expected.pairs[0] = (object_store_cs_pair_t){0, 1, true, 3200, 950U, 1U, 12U};
    expected.pairs[1] = (object_store_cs_pair_t){0, 2, false, 0, 0U, 5U, 0U};

    assert(object_store_set_cs_pairs("OBJ_0001", &expected) == ESP_OK);
    assert(strstr(persisted_json, "\"cs_pairs\"") != NULL);

    object_store_profile_t reopened;
    assert(object_store_get_profile("OBJ_0001", &reopened) == ESP_OK);
    assert_cs_pairs_equal(&expected, &reopened.cs_pairs);

    /* Setting geometry afterward must not clobber the already-persisted
     * cs_pairs (both are independent top-level object.json keys). */
    object_store_geometry_t geometry = {0};
    geometry.pods[0] = (object_store_pod_geometry_t){true, true, 1, 2, 3, 99U};
    assert(object_store_set_geometry("OBJ_0001", &geometry) == ESP_OK);

    object_store_profile_t after_geometry;
    assert(object_store_get_profile("OBJ_0001", &after_geometry) == ESP_OK);
    assert_cs_pairs_equal(&expected, &after_geometry.cs_pairs);
    assert(after_geometry.geometry.pods[0].computed);
}

static void test_cs_pairs_clear_with_null(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_cs_pairs_t populated = {0};
    populated.pair_count = 1U;
    populated.pairs[0] = (object_store_cs_pair_t){0, 3, true, 1000, 800U, 1U, 20U};
    assert(object_store_set_cs_pairs("OBJ_0001", &populated) == ESP_OK);

    assert(object_store_set_cs_pairs("OBJ_0001", NULL) == ESP_OK);

    object_store_profile_t reopened;
    assert(object_store_get_profile("OBJ_0001", &reopened) == ESP_OK);
    assert(reopened.cs_pairs.pair_count == 0U);
}

static void test_old_and_malformed_cs_pairs_are_safe(void)
{
    set_metadata(
        "{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\","
        "\"cs_pairs\":{\"pairs\":["
        "{\"node_a\":0,\"node_b\":1,\"valid\":true,\"distance_mm\":100,"
        "\"confidence\":900,\"attempts\":1,\"valid_samples\":10},"
        "{\"node_a\":0,\"node_b\":2,\"distance_mm\":\"bad\"}]}}"
    );

    object_store_profile_t profile;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    /* The malformed second entry is skipped entirely, not partially parsed. */
    assert(profile.cs_pairs.pair_count == 1U);
    assert(profile.cs_pairs.pairs[0].node_a == 0U);
    assert(profile.cs_pairs.pairs[0].node_b == 1U);
    assert(profile.cs_pairs.pairs[0].valid);
    assert(profile.cs_pairs.pairs[0].distance_mm == 100);
    assert(profile.cs_pairs.pairs[0].confidence == 900U);
}

static void test_object_with_no_cs_pairs_key_defaults_to_empty(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_profile_t profile;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(profile.cs_pairs.pair_count == 0U);
}

int main(void)
{
    test_cs_pairs_write_close_reopen_round_trip();
    test_cs_pairs_clear_with_null();
    test_old_and_malformed_cs_pairs_are_safe();
    test_object_with_no_cs_pairs_key_defaults_to_empty();
    puts("OBJECT_STORE_CS_PAIRS_ROUND_TRIP_HOST_PASS");
    return 0;
}
