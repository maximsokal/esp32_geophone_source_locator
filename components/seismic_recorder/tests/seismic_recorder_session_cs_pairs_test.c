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


static void test_valid_cs_pairs_round_trip(void)
{
    set_session_json(
        "{\"cs_pairs\":{\"pairs\":["
        "{\"node_a\":0,\"node_b\":1,\"valid\":true,\"distance_mm\":3200,"
        "\"confidence\":950,\"attempts\":1,\"valid_samples\":12},"
        "{\"node_a\":0,\"node_b\":2,\"valid\":false,\"distance_mm\":0,"
        "\"confidence\":0,\"attempts\":5,\"valid_samples\":0}"
        "]}}"
    );

    object_store_cs_pairs_t cs_pairs;
    memset(&cs_pairs, 0xAA, sizeof(cs_pairs));
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &cs_pairs) == ESP_OK);

    assert(cs_pairs.pair_count == 2U);
    assert(cs_pairs.pairs[0].node_a == 0U && cs_pairs.pairs[0].node_b == 1U);
    assert(cs_pairs.pairs[0].valid);
    assert(cs_pairs.pairs[0].distance_mm == 3200);
    assert(cs_pairs.pairs[0].confidence == 950U);
    assert(cs_pairs.pairs[0].attempts == 1U);
    assert(cs_pairs.pairs[0].valid_samples == 12U);

    assert(cs_pairs.pairs[1].node_a == 0U && cs_pairs.pairs[1].node_b == 2U);
    assert(!cs_pairs.pairs[1].valid);
    assert(cs_pairs.pairs[1].attempts == 5U);
}


static void test_missing_cs_pairs_key_defaults_to_empty(void)
{
    /* A session recorded before MAX-148 ever existed. */
    set_session_json("{\"object_id\":\"OBJ_0001\",\"session_id\":\"S000001\"}");

    object_store_cs_pairs_t cs_pairs;
    memset(&cs_pairs, 0xAA, sizeof(cs_pairs));
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &cs_pairs) == ESP_OK);
    assert(cs_pairs.pair_count == 0U);
}


static void test_malformed_pair_entry_is_skipped_not_fatal(void)
{
    set_session_json(
        "{\"cs_pairs\":{\"pairs\":["
        "{\"node_a\":0,\"node_b\":1,\"valid\":true,\"distance_mm\":\"not-a-number\"},"
        "{\"node_a\":0,\"node_b\":3,\"valid\":true,\"distance_mm\":1500,"
        "\"confidence\":700,\"attempts\":2,\"valid_samples\":8}"
        "]}}"
    );

    object_store_cs_pairs_t cs_pairs;
    memset(&cs_pairs, 0xAA, sizeof(cs_pairs));
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &cs_pairs) == ESP_OK);

    /* Only the well-formed entry survives -- the malformed one is skipped
     * entirely, not partially parsed with fabricated fields. */
    assert(cs_pairs.pair_count == 1U);
    assert(cs_pairs.pairs[0].node_a == 0U && cs_pairs.pairs[0].node_b == 3U);
    assert(cs_pairs.pairs[0].distance_mm == 1500);
}


static void test_missing_file_is_a_real_error(void)
{
    s_session_exists = false;
    object_store_cs_pairs_t cs_pairs;
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &cs_pairs) == ESP_ERR_NOT_FOUND);
    s_session_exists = true;
}


static void test_unparseable_json_is_a_real_error(void)
{
    set_session_json("not-json-at-all");
    object_store_cs_pairs_t cs_pairs;
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", &cs_pairs) == ESP_ERR_INVALID_RESPONSE);
}


static void test_rejects_null_arguments(void)
{
    object_store_cs_pairs_t cs_pairs;
    assert(seismic_recorder_read_session_cs_pairs(NULL, "S000001", &cs_pairs) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", NULL, &cs_pairs) == ESP_ERR_INVALID_ARG);
    assert(seismic_recorder_read_session_cs_pairs("OBJ_0001", "S000001", NULL) == ESP_ERR_INVALID_ARG);
}


int main(void)
{
    test_valid_cs_pairs_round_trip();
    test_missing_cs_pairs_key_defaults_to_empty();
    test_malformed_pair_entry_is_skipped_not_fatal();
    test_missing_file_is_a_real_error();
    test_unparseable_json_is_a_real_error();
    test_rejects_null_arguments();
    puts("seismic recorder session cs pairs tests passed");
    return 0;
}
