#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cjson_alloc_guard.h"
#include "object_store_internal.h"
#include "storage_manager.h"

static char persisted_json[8192];
static int64_t fake_time_us;
static unsigned s_metadata_read_calls;
static unsigned s_object_exists_calls;
static unsigned s_path_exists_calls;
static esp_err_t s_metadata_read_error;
static bool s_storage_ready = true;
static esp_err_t s_path_status_error;
static bool s_settings_present;
static char s_settings_json[256];
static unsigned s_settings_writes;

extern int cJSON_stub_fail_alloc_countdown;
extern void (*cJSON_stub_on_alloc_failure)(void);

static bool is_settings_path(const char *path)
{
    return path != NULL && strstr(path, "/settings.json") != NULL;
}

/* The legacy boolean probe cannot express a failed stat: it reports "absent". */
bool storage_manager_path_exists(const char *path)
{
    if (path != NULL && strstr(path, "/object.json") != NULL) ++s_path_exists_calls;
    if (is_settings_path(path)) return s_settings_present && s_path_status_error == ESP_OK;
    return path != NULL && strstr(path, "/object.json") != NULL;
}

esp_err_t storage_manager_path_status(const char *path, bool *exists)
{
    if (path == NULL || exists == NULL) return ESP_ERR_INVALID_ARG;
    if (s_path_status_error != ESP_OK) return s_path_status_error;
    *exists = is_settings_path(path)
        ? s_settings_present
        : strstr(path, "/object.json") != NULL;
    return ESP_OK;
}

bool storage_manager_is_ready(void)
{
    return s_storage_ready;
}

void storage_manager_operation_end(void)
{
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    if (path == NULL || buffer == NULL || buffer_size == 0U) return ESP_ERR_INVALID_ARG;
    if (is_settings_path(path)) {
        if (!s_settings_present) return ESP_ERR_NOT_FOUND;
        if (strlen(s_settings_json) + 1U > buffer_size) return ESP_ERR_INVALID_SIZE;
        memcpy(buffer, s_settings_json, strlen(s_settings_json) + 1U);
        return ESP_OK;
    }
    if (strstr(path, "/object.json") != NULL) {
        ++s_metadata_read_calls;
        if (s_metadata_read_error != ESP_OK) return s_metadata_read_error;
    }
    if (!storage_manager_path_exists(path)) return ESP_ERR_NOT_FOUND;
    if (strlen(persisted_json) + 1U > buffer_size) return ESP_ERR_INVALID_SIZE;
    memcpy(buffer, persisted_json, strlen(persisted_json) + 1U);
    return ESP_OK;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) return ESP_ERR_INVALID_ARG;
    if (is_settings_path(path)) {
        if (strlen(text) + 1U > sizeof(s_settings_json)) return ESP_ERR_INVALID_SIZE;
        memcpy(s_settings_json, text, strlen(text) + 1U);
        s_settings_present = true;
        ++s_settings_writes;
        return ESP_OK;
    }
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
    ++s_object_exists_calls;
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
    object_store_profile_cache_reset();
    s_metadata_read_calls = 0U;
    s_object_exists_calls = 0U;
    s_path_exists_calls = 0U;
    s_metadata_read_error = ESP_OK;
    s_storage_ready = true;
    s_path_status_error = ESP_OK;
    s_settings_present = false;
    s_settings_json[0] = '\0';
    s_settings_writes = 0U;
    cJSON_stub_fail_alloc_countdown = -1;
    cJSON_stub_on_alloc_failure = cjson_alloc_guard_note_failure;
    assert(strlen(json) + 1U < sizeof(persisted_json));
    memcpy(persisted_json, json, strlen(json) + 1U);
}

static void assert_location_equal(
    const object_store_location_t *expected,
    const object_store_location_t *actual
);


static void test_profile_cache_reads_once_and_reloads_after_mutation(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");

    object_store_profile_t profile;
    for (unsigned read = 0U; read < 5U; ++read) {
        assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    }
    assert(s_metadata_read_calls == 1U);
    assert(s_object_exists_calls == 1U);
    assert(s_path_exists_calls == 1U);

    object_store_location_t expected = {
        .valid = true,
        .latitude = 51.5,
        .longitude = -0.12,
        .accuracy_m = 2.0f,
        .captured_at_ms = 77U,
    };
    (void)snprintf(expected.source, sizeof(expected.source), "PHONE_GPS");
    assert(object_store_set_location("OBJ_0001", &expected) == ESP_OK);

    const unsigned reads_after_write = s_metadata_read_calls;
    const unsigned exists_after_write = s_object_exists_calls;
    const unsigned path_stats_after_write = s_path_exists_calls;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert_location_equal(&expected, &profile.location);
    assert(s_metadata_read_calls == reads_after_write + 1U);
    assert(s_object_exists_calls == exists_after_write + 1U);
    assert(s_path_exists_calls == path_stats_after_write + 1U);

    const unsigned reads_after_reload = s_metadata_read_calls;
    const unsigned exists_after_reload = s_object_exists_calls;
    const unsigned path_stats_after_reload = s_path_exists_calls;
    for (unsigned read = 0U; read < 5U; ++read) {
        assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    }
    assert(s_metadata_read_calls == reads_after_reload);
    assert(s_object_exists_calls == exists_after_reload);
    assert(s_path_exists_calls == path_stats_after_reload);
}

static void test_profile_cache_does_not_cache_errors_and_drops_on_reset_or_unmount(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");
    object_store_profile_t profile;

    s_metadata_read_error = ESP_FAIL;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_FAIL);
    assert(s_metadata_read_calls == 1U);
    s_metadata_read_error = ESP_OK;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(s_metadata_read_calls == 2U);
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(s_metadata_read_calls == 2U);

    s_storage_ready = false;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_ERR_INVALID_STATE);
    assert(s_metadata_read_calls == 2U);
    s_storage_ready = true;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(s_metadata_read_calls == 3U);

    object_store_profile_cache_reset();
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);
    assert(s_metadata_read_calls == 4U);
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
        assert(expected->pods[i].residual_mm == actual->pods[i].residual_mm);
        assert(expected->pods[i].uncertainty_mm == actual->pods[i].uncertainty_mm);
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
    expected.pods[0] = (object_store_pod_geometry_t){.computed = true, .leveled = true, .x_mm = -123, .y_mm = 456, .z_mm = 789, .residual_mm = 17, .uncertainty_mm = 42, .computed_at_ms = 1001U};
    expected.pods[1] = (object_store_pod_geometry_t){.computed = true, .leveled = false, .x_mm = 1200, .y_mm = -340, .z_mm = 55, .computed_at_ms = 1002U};
    expected.pods[3] = (object_store_pod_geometry_t){.computed = true, .leveled = true, .x_mm = 0, .y_mm = 0, .z_mm = 2500, .computed_at_ms = 1003U};

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

static void test_transient_settings_probe_failure_does_not_reset_cable_lengths(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");
    s_settings_present = true;
    (void)snprintf(
        s_settings_json,
        sizeof(s_settings_json),
        "{\"cable_lengths_m\":[12.5,13.5,14.5,15.5]}"
    );

    /* settings.json exists, but the existence probe fails once. */
    s_path_status_error = ESP_FAIL;
    assert(object_store_set_cable_default(0U, 20.0f) == ESP_FAIL);
    assert(!s_settings_cache_valid);
    assert(s_settings_writes == 0U);

    /* Access returns: the stored lengths of the other pods must survive. */
    s_path_status_error = ESP_OK;
    assert(object_store_set_cable_default(0U, 20.0f) == ESP_OK);
    assert(s_settings_writes == 1U);
    float lengths[OBJECT_STORE_POD_COUNT];
    assert(object_store_settings_load_locked(lengths) == ESP_OK);
    assert(fabsf(lengths[0] - 20.0f) < 1e-6f);
    assert(fabsf(lengths[1] - 13.5f) < 1e-6f);
    assert(fabsf(lengths[2] - 14.5f) < 1e-6f);
    assert(fabsf(lengths[3] - 15.5f) < 1e-6f);
    assert(strstr(s_settings_json, "13.5") != NULL);
    assert(strstr(s_settings_json, "15.5") != NULL);
}

static void test_genuinely_absent_settings_still_mean_defaults(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");
    float lengths[OBJECT_STORE_POD_COUNT];
    assert(object_store_settings_load_locked(lengths) == ESP_OK);
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(fabsf(lengths[i] - OBJECT_STORE_DEFAULT_CABLE_LENGTH_M) < 1e-6f);
    }
    assert(s_settings_cache_valid);
}

static void test_cjson_out_of_memory_is_not_reported_as_corruption(void)
{
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");
    object_store_profile_t profile;

    cJSON_stub_fail_alloc_countdown = 0;
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_ERR_NO_MEM);
    assert(!object_store_profile_error_is_omittable(ESP_ERR_NO_MEM));
    assert(object_store_profile_error_is_omittable(ESP_ERR_INVALID_RESPONSE));

    /* Memory is back: the same object is readable again. */
    assert(object_store_get_profile("OBJ_0001", &profile) == ESP_OK);

    /* Truly malformed metadata remains INVALID_RESPONSE (skippable). */
    test_interrupted_update_is_rejected();

    /* Out-of-memory while parsing settings.json is not cached as defaults. */
    set_metadata("{\"schema_version\":1,\"id\":\"OBJ_0001\",\"name\":\"FIELD\"}");
    s_settings_present = true;
    (void)snprintf(s_settings_json, sizeof(s_settings_json), "{\"cable_lengths_m\":[12.5,13.5,14.5,15.5]}");
    float lengths[OBJECT_STORE_POD_COUNT];
    cJSON_stub_fail_alloc_countdown = 0;
    assert(object_store_settings_load_locked(lengths) == ESP_ERR_NO_MEM);
    assert(!s_settings_cache_valid);
    assert(object_store_settings_load_locked(lengths) == ESP_OK);
    assert(fabsf(lengths[1] - 13.5f) < 1e-6f);
}

int main(void)
{
    test_profile_cache_reads_once_and_reloads_after_mutation();
    test_profile_cache_does_not_cache_errors_and_drops_on_reset_or_unmount();
    test_geometry_write_close_reopen_round_trip();
    test_old_and_partial_geometry_are_safe();
    test_phone_location_write_close_reopen_round_trip();
    test_phone_location_rejects_invalid_coordinates_and_accuracy();
    test_interrupted_update_is_rejected();
    test_transient_settings_probe_failure_does_not_reset_cable_lengths();
    test_genuinely_absent_settings_still_mean_defaults();
    test_cjson_out_of_memory_is_not_reported_as_corruption();
    puts("OBJECT_STORE_PROFILE_ROUND_TRIP_HOST_PASS");
    return 0;
}
