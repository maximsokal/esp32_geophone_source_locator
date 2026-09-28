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

esp_err_t object_store_internal_begin_file_operation(void) { return ESP_OK; }
void object_store_internal_end_file_operation(void) {}

bool object_store_fs_object_exists(const char *object_id)
{
    return object_id != NULL && strcmp(object_id, "OBJ_0001") == 0;
}

esp_err_t storage_manager_atomic_write_text(const char *path, const char *text)
{
    assert(strstr(path, "/OBJ_0001/calc_settings.json") != NULL);
    if (strlen(text) >= sizeof(s_settings)) return ESP_ERR_INVALID_SIZE;
    strcpy(s_settings, text);
    return ESP_OK;
}

esp_err_t storage_manager_read_text(const char *path, char *buffer, size_t buffer_size)
{
    assert(strstr(path, "/OBJ_0001/calc_settings.json") != NULL);
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
    const char *json = "{\"propagation_speed_mps\":340,\"time_budget_s\":30}";
    assert(object_store_set_calc_settings_json("OBJ_0001", json) == ESP_OK);
    char *loaded = NULL;
    assert(object_store_get_calc_settings_json("OBJ_0001", &loaded) == ESP_OK);
    assert(loaded != NULL && strcmp(loaded, json) == 0);
    free(loaded);
    assert(object_store_get_calc_settings_json("OBJ_0002", &loaded) == ESP_ERR_NOT_FOUND);
    assert(loaded == NULL);
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
    test_scan_paths_and_stat_count();
    return 0;
}
