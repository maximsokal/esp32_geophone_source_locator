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

static void assert_geometry_equal(
    const object_store_geometry_t *expected,
    const object_store_geometry_t *actual
)
{
    assert(expected->anchored == actual->anchored);
    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(expected->pods[i].computed == actual->pods[i].computed);
        assert(expected->pods[i].leveled == actual->pods[i].leveled);
        assert(expected->pods[i].x_mm == actual->pods[i].x_mm);
        assert(expected->pods[i].y_mm == actual->pods[i].y_mm);
        assert(expected->pods[i].z_mm == actual->pods[i].z_mm);
        assert(expected->pods[i].computed_at_ms == actual->pods[i].computed_at_ms);
    }
}


static void assert_location_equal(
    const object_store_location_t *expected,
    const object_store_location_t *actual
)
{
    assert(expected->valid == actual->valid);
    assert(fabs(expected->latitude - actual->latitude) < 1e-9);
    assert(fabs(expected->longitude - actual->longitude) < 1e-9);
    assert(fabsf(expected->accuracy_m - actual->accuracy_m) < 1e-6f);
    assert(expected->altitude_valid == actual->altitude_valid);
    assert(fabs(expected->altitude_m - actual->altitude_m) < 1e-9);
    assert(fabsf(expected->altitude_accuracy_m - actual->altitude_accuracy_m) < 1e-6f);
    assert(expected->captured_at_ms == actual->captured_at_ms);
    assert(strcmp(expected->source, actual->source) == 0);
}

static void test_geometry_write_close_reopen_round_trip(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_profile_t before;
    assert(object_store_get_profile("OBJ_0001", &before) == ESP_OK);
    assert(!before.geometry.anchored);
    for (size_t i = 0; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(!before.geometry.pods[i].computed);
    }

    object_store_geometry_t expected = {0};
    expected.anchored = true;
    expected.pods[0] = (object_store_pod_geometry_t){true, true, -123, 456, 789, 1001U};
    expected.pods[1] = (object_store_pod_geometry_t){true, false, 1200, -340, 55, 1002U};
    expected.pods[3] = (object_store_pod_geometry_t){true, true, 0, 0, 2500, 1003U};

    assert(object_store_set_geometry("OBJ_0001", &expected) == ESP_OK);
    assert(strstr(persisted_json, "\"geometry\"") != NULL);

    object_store_profile_t reopened;
    assert(object_store_get_profile("OBJ_0001", &reopened) == ESP_OK);
    assert_geometry_equal(&expected, &reopened.geometry);
}

static void test_old_and_partial_geometry_are_safe(void)
{
    set_metadata(
        "{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\","
        "\"geometry\":{\"anchored\":true,\"pods\":["
        "{\"computed\":true,\"leveled\":true,\"x_mm\":12},"
        "{\"computed\":true,\"leveled\":false,\"x_mm\":10,\"y_mm\":20,\"z_mm\":30,\"computed_at_ms\":44},"
        "{\"computed\":true,\"x_mm\":\"bad\",\"y_mm\":1,\"z_mm\":2}]}}"
    );

    object_store_profile_t profile;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(profile.geometry.anchored);
    assert(!profile.geometry.pods[0].computed);
    assert(profile.geometry.pods[1].computed);
    assert(profile.geometry.pods[1].x_mm == 10);
    assert(profile.geometry.pods[1].y_mm == 20);
    assert(profile.geometry.pods[1].z_mm == 30);
    assert(profile.geometry.pods[1].computed_at_ms == 44U);
    assert(!profile.geometry.pods[2].computed);
    assert(!profile.geometry.pods[3].computed);
}


static void test_phone_location_write_close_reopen_round_trip(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_location_t expected = {
        .valid = true,
        .latitude = 50.4501,
        .longitude = 30.5234,
        .accuracy_m = 3.2f,
        .altitude_valid = true,
        .altitude_m = 179.4,
        .altitude_accuracy_m = 5.5f,
        .captured_at_ms = 123456789U,
    };
    (void)snprintf(expected.source, sizeof(expected.source), "PHONE_GPS");

    assert(object_store_set_location("OBJ_0001", &expected) == ESP_OK);
    assert(strstr(persisted_json, "\"location\"") != NULL);
    assert(strstr(persisted_json, "PHONE_GPS") != NULL);

    object_store_profile_t reopened;
    assert(object_store_get_profile("OBJ_0001", &reopened) == ESP_OK);
    assert_location_equal(&expected, &reopened.location);
}


static void test_phone_location_rejects_invalid_coordinates_and_accuracy(void)
{
    object_store_location_t invalid = {
        .valid = true,
        .latitude = 91.0,
        .longitude = 30.0,
        .accuracy_m = 1.0f,
    };
    assert(object_store_set_location("OBJ_0001", &invalid) == ESP_ERR_INVALID_ARG);

    invalid.latitude = 50.0;
    invalid.longitude = -181.0;
    assert(object_store_set_location("OBJ_0001", &invalid) == ESP_ERR_INVALID_ARG);

    invalid.longitude = 30.0;
    invalid.accuracy_m = -1.0f;
    assert(object_store_set_location("OBJ_0001", &invalid) == ESP_ERR_INVALID_ARG);

    invalid.accuracy_m = 1.0f;
    invalid.latitude = NAN;
    assert(object_store_set_location("OBJ_0001", &invalid) == ESP_ERR_INVALID_ARG);
}

static void test_interrupted_update_is_rejected(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\"");
    object_store_profile_t profile;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_ERR_INVALID_RESPONSE);
}

int main(void)
{
    test_geometry_write_close_reopen_round_trip();
    test_old_and_partial_geometry_are_safe();
    test_phone_location_write_close_reopen_round_trip();
    test_phone_location_rejects_invalid_coordinates_and_accuracy();
    test_interrupted_update_is_rejected();
    puts("OBJECT_STORE_PROFILE_ROUND_TRIP_HOST_PASS");
    return 0;
}
