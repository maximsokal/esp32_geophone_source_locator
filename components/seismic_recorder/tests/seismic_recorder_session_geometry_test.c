#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "seismic_recorder_internal.h"
#include "storage_manager.h"


static char s_persisted_json[4096];
static bool s_session_exists = true;


esp_err_t seismic_recorder_fs_build_session_path(
    const char *object_id,
    const char *session_id,
    char *path,
    size_t path_size
)
{
    const int written = snprintf(
        path, path_size, "%s/%s/sessions/%s",
        STORAGE_MANAGER_OBJECTS_PATH, object_id, session_id
    );
    return written < 0 || written >= (int)path_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}


esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    (void)path;
    if (!s_session_exists) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlen(s_persisted_json) + 1U > buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, s_persisted_json, strlen(s_persisted_json) + 1U);
    return ESP_OK;
}


esp_err_t storage_manager_read_text_known_present(const char *path, char *buffer, size_t buffer_size)
{
    return storage_manager_read_text(path, buffer, buffer_size);
}


esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    (void)path;
    if (strlen(text) + 1U > sizeof(s_persisted_json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_persisted_json, text, strlen(text) + 1U);
    return ESP_OK;
}


const char *seismic_recorder_stop_stage_name(seismic_recorder_stop_stage_t stage)
{
    (void)stage;
    return "test";
}


static void set_session_json(const char *json)
{
    snprintf(s_persisted_json, sizeof(s_persisted_json), "%s", json);
    s_session_exists = true;
}


static void test_valid_geometry_round_trips(void)
{
    set_session_json(
        "{\"geometry\":{\"anchored\":false,\"pods\":["
        "{\"computed\":true,\"leveled\":true,\"x_mm\":300,\"y_mm\":0,\"z_mm\":10,\"computed_at_ms\":123456},"
        "{\"computed\":true,\"leveled\":false,\"x_mm\":150,\"y_mm\":200,\"z_mm\":5,\"computed_at_ms\":123457},"
        "{\"computed\":false,\"leveled\":false,\"x_mm\":0,\"y_mm\":0,\"z_mm\":0,\"computed_at_ms\":0},"
        "{\"computed\":true,\"leveled\":true,\"x_mm\":-150,\"y_mm\":-200,\"z_mm\":-5,\"computed_at_ms\":123458}"
        "]}}"
    );

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(!geometry.anchored);
    assert(geometry.pods[0].computed && geometry.pods[0].leveled);
    assert(geometry.pods[0].x_mm == 300 && geometry.pods[0].y_mm == 0 && geometry.pods[0].z_mm == 10);
    assert(geometry.pods[0].computed_at_ms == 123456ULL);

    assert(geometry.pods[1].computed && !geometry.pods[1].leveled);
    assert(geometry.pods[1].x_mm == 150 && geometry.pods[1].y_mm == 200);

    assert(!geometry.pods[2].computed);

    assert(geometry.pods[3].computed && geometry.pods[3].leveled);
    assert(geometry.pods[3].x_mm == -150 && geometry.pods[3].y_mm == -200 && geometry.pods[3].z_mm == -5);
}


static void test_missing_geometry_key_defaults_to_zeroed(void)
{
    /* A session recorded before MAX-166 ever existed. */
    set_session_json("{\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\"}");

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(!geometry.anchored);
    for (size_t i = 0U; i < OBJECT_STORE_POD_COUNT; ++i) {
        assert(!geometry.pods[i].computed);
    }
}


static void test_malformed_pod_entry_is_skipped_not_fatal(void)
{
    set_session_json(
        "{\"geometry\":{\"anchored\":true,\"pods\":["
        "{\"computed\":true,\"leveled\":true,\"x_mm\":\"not-a-number\",\"y_mm\":0,\"z_mm\":0},"
        "{\"computed\":true,\"leveled\":true,\"x_mm\":10,\"y_mm\":20,\"z_mm\":30,\"computed_at_ms\":999}"
        "]}}"
    );

    object_store_geometry_t geometry;
    memset(&geometry, 0xAA, sizeof(geometry));
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_OK);

    assert(geometry.anchored);
    assert(!geometry.pods[0].computed);
    assert(geometry.pods[1].computed);
    assert(geometry.pods[1].x_mm == 10 && geometry.pods[1].y_mm == 20 && geometry.pods[1].z_mm == 30);
}


static void test_missing_file_is_a_real_error(void)
{
    s_session_exists = false;
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_ERR_NOT_FOUND);
    s_session_exists = true;
}


static void test_unparseable_json_is_a_real_error(void)
{
    set_session_json("not-json-at-all");
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", &geometry) == ESP_ERR_INVALID_RESPONSE);
}


static void test_rejects_null_arguments(void)
{
    object_store_geometry_t geometry;
    assert(seismic_recorder_read_session_geometry(NULL, "S000001", &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_geometry("OBJ_0001", NULL, &geometry) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_geometry("OBJ_0001", "S000001", NULL) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_valid_geometry_round_trips();
    test_missing_geometry_key_defaults_to_zeroed();
    test_malformed_pod_entry_is_skipped_not_fatal();
    test_missing_file_is_a_real_error();
    test_unparseable_json_is_a_real_error();
    test_rejects_null_arguments();
    puts("seismic recorder session geometry tests passed");
    return 0;
}
