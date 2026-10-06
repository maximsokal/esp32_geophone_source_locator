#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "object_store.h"
#include "object_store_internal.h"
#include "storage_manager.h"

static char s_settings[OBJECT_STORE_CALC_SETTINGS_JSON_SIZE];
static bool s_session_scan;
static bool s_staged_scan;
static unsigned s_stat_calls;
static unsigned s_settings_read_calls;
static unsigned s_object_exists_calls;
static unsigned s_settings_write_calls;
static esp_err_t s_settings_read_error;
static bool s_storage_ready = true;

esp_err_t object_store_internal_begin_file_operation(void) { return ESP_OK; }
void object_store_internal_end_file_operation(void) {}
esp_err_t object_store_internal_begin_state_operation(void) { return ESP_OK; }
void object_store_internal_end_state_operation(void) {}
void storage_manager_operation_end(void) {}
bool storage_manager_is_ready(void) { return s_storage_ready; }
void object_store_profile_cache_note_calc_settings_saved_locked(
    const char *object_id,
    uint64_t generation)
{
    (void)object_id;
    (void)generation;
}

bool object_store_fs_object_exists(const char *object_id)
{
    ++s_object_exists_calls;
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    ++s_settings_write_calls;
    assert(strstr(path, "/OBJ_0001/calc_settings.json") != NULL);
    if (strlen(text) >= sizeof(s_settings)) return ESP_ERR_INVALID_SIZE;
    strcpy(s_settings, text);
    return ESP_OK;
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    assert(strstr(path, "/OBJ_0001/calc_settings.json") != NULL);
    ++s_settings_read_calls;
    if (s_settings_read_error != ESP_OK) return s_settings_read_error;
    if (s_settings[0] == '\0') return ESP_ERR_NOT_FOUND;
    if (strlen(s_settings) >= buffer_size) return ESP_ERR_INVALID_SIZE;
    strcpy(buffer, s_settings);
    return ESP_OK;
}

int object_store_fake_stat(const char *path, struct stat *info)
{
    ++s_stat_calls;
    if ((strstr(path, "/sessions/S000001/scan-v1.bin") != NULL && s_session_scan) ||
        (strstr(path, "/staging/scan-v1.bin") != NULL && s_staged_scan)) {
        info->st_mode = S_IFREG;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static void test_calc_settings_round_trip(void)
{
    object_store_cache_reset();
    s_settings[0] = '\0';
    s_settings_read_calls = 0U;
    s_object_exists_calls = 0U;
    s_settings_write_calls = 0U;
    s_settings_read_error = ESP_OK;
    s_storage_ready = true;
    const char *json = "{\"propagation_speed_mps\":340,\"time_budget_s\":30}";
    assert(object_store_set_calc_settings_json("OBJ_0001", json) == ESP_OK);
    char *loaded = NULL;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    assert(loaded != NULL && strcmp(loaded, json) == 0);
    free(loaded);
    assert(object_store_get_calc_settings_json("OBJ_0002", &loaded) == ESP_ERR_NOT_FOUND);
    assert(loaded == NULL);
}

static void test_absent_calc_settings_cache_and_save_invalidation(void)
{
    object_store_cache_reset();
    s_settings[0] = '\0';
    s_settings_read_calls = 0U;
    s_object_exists_calls = 0U;
    s_settings_read_error = ESP_OK;
    s_storage_ready = true;

    char *loaded = NULL;
    for (unsigned read = 0U; read < 6U; ++read) {
        assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_ERR_NOT_FOUND);
        assert(loaded == NULL);
    }
    assert(s_settings_read_calls == 1U);
    assert(s_object_exists_calls == 1U);

    const char *saved = "{\"propagation_speed_mps\":355,\"time_budget_s\":45}";
    assert(object_store_set_calc_settings_json("OBJ_0001", saved) == ESP_OK);
    const unsigned reads_after_save = s_settings_read_calls;
    const unsigned exists_after_save = s_object_exists_calls;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    assert(loaded != NULL && strcmp(loaded, saved) == 0);
    free(loaded);
    assert(s_settings_read_calls == reads_after_save + 1U);
    assert(s_object_exists_calls == exists_after_save + 1U);

    const unsigned reads_after_reload = s_settings_read_calls;
    const unsigned exists_after_reload = s_object_exists_calls;
    for (unsigned read = 0U; read < 6U; ++read) {
        assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
        assert(loaded != NULL && strcmp(loaded, saved) == 0);
        free(loaded);
    }
    assert(s_settings_read_calls == reads_after_reload);
    assert(s_object_exists_calls == exists_after_reload);
}

static void test_calc_settings_errors_retry_and_storage_generation_reset(void)
{
    object_store_cache_reset();
    (void)snprintf(s_settings, sizeof(s_settings), "{\"value\":1}");
    s_settings_read_calls = 0U;
    s_object_exists_calls = 0U;
    s_settings_read_error = ESP_FAIL;
    s_storage_ready = true;

    char *loaded = NULL;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_FAIL);
    assert(loaded == NULL && s_settings_read_calls == 1U);
    s_settings_read_error = ESP_OK;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    assert(loaded != NULL && strcmp(loaded, s_settings) == 0);
    free(loaded);
    assert(s_settings_read_calls == 2U);

    s_storage_ready = false;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_ERR_INVALID_STATE);
    assert(loaded == NULL && s_settings_read_calls == 2U);
    s_storage_ready = true;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    free(loaded);
    assert(s_settings_read_calls == 3U);

    object_store_cache_reset();
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    free(loaded);
    assert(s_settings_read_calls == 4U);
}

static void test_scan_paths_and_stat_count(void)
{
    bool present = true;
    s_session_scan = true;
    assert(object_store_scan_present("OBJ_0001", "S000001", &present) == ESP_OK);
    assert(present && s_stat_calls == 1U);
    s_session_scan = false;
    s_staged_scan = true;
    assert(object_store_scan_present("OBJ_0001", "", &present) == ESP_OK);
    assert(present && s_stat_calls == 2U);
    s_staged_scan = false;
    assert(object_store_scan_present("OBJ_0001", "", &present) == ESP_OK);
    assert(!present && s_stat_calls == 3U);
    assert(object_store_scan_present("OBJ_0001", "../bad", &present) == ESP_ERR_INVALID_ARG);
    assert(s_stat_calls == 3U);
}

int main(void)
{
    test_calc_settings_round_trip();
    test_absent_calc_settings_cache_and_save_invalidation();
    test_calc_settings_errors_retry_and_storage_generation_reset();
    test_scan_paths_and_stat_count();
    return 0;
}
