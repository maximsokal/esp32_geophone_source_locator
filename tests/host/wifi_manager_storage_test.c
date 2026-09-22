#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "nvs.h"
#include "wifi_manager.h"
#include "wifi_manager_credentials.h"
#include "wifi_manager_profile.h"
#include "wifi_manager_runtime.h"

static wifi_config_t s_wifi_config;
static esp_err_t s_wifi_get_result;
static esp_err_t s_wifi_set_result;
static unsigned s_wifi_get_calls;
static unsigned s_wifi_set_calls;
static bool s_nvs_namespace_exists;
static bool s_nvs_blob_exists;
static uint8_t s_nvs_mac[6];
static size_t s_nvs_mac_length;
static esp_err_t s_nvs_open_read_result;
static esp_err_t s_nvs_open_write_result;
static esp_err_t s_nvs_get_result;
static esp_err_t s_nvs_set_result;
static esp_err_t s_nvs_commit_result;
static esp_err_t s_nvs_erase_result;
static unsigned s_nvs_open_calls;
static unsigned s_nvs_close_calls;
static unsigned s_nvs_commit_calls;

static void reset_fakes(void)
{
    memset(&s_wifi_config, 0, sizeof(s_wifi_config));
    s_wifi_get_result = ESP_OK;
    s_wifi_set_result = ESP_OK;
    s_wifi_get_calls = 0U;
    s_wifi_set_calls = 0U;
    s_nvs_namespace_exists = false;
    s_nvs_blob_exists = false;
    memset(s_nvs_mac, 0, sizeof(s_nvs_mac));
    s_nvs_mac_length = 0U;
    s_nvs_open_read_result = ESP_OK;
    s_nvs_open_write_result = ESP_OK;
    s_nvs_get_result = ESP_OK;
    s_nvs_set_result = ESP_OK;
    s_nvs_commit_result = ESP_OK;
    s_nvs_erase_result = ESP_OK;
    s_nvs_open_calls = 0U;
    s_nvs_close_calls = 0U;
    s_nvs_commit_calls = 0U;
    wifi_manager_runtime_clear_setup_client();
}

esp_err_t esp_wifi_get_config(int interface_id, wifi_config_t *config)
{
    assert(interface_id == WIFI_IF_STA);
    assert(config != NULL);
    ++s_wifi_get_calls;
    if (s_wifi_get_result == ESP_OK) {
        *config = s_wifi_config;
    }
    return s_wifi_get_result;
}

esp_err_t esp_wifi_set_config(int interface_id, const wifi_config_t *config)
{
    assert(interface_id == WIFI_IF_STA);
    assert(config != NULL);
    ++s_wifi_set_calls;
    if (s_wifi_set_result == ESP_OK) {
        s_wifi_config = *config;
    }
    return s_wifi_set_result;
}

esp_err_t nvs_open(
    const char *namespace_name,
    int open_mode,
    nvs_handle_t *handle
)
{
    assert(strcmp(namespace_name, "wifi_profile") == 0);
    assert(handle != NULL);
    ++s_nvs_open_calls;
    if (open_mode == NVS_READONLY) {
        if (s_nvs_open_read_result != ESP_OK) return s_nvs_open_read_result;
        if (!s_nvs_namespace_exists) return ESP_ERR_NVS_NOT_FOUND;
    }
    else {
        if (s_nvs_open_write_result != ESP_OK) return s_nvs_open_write_result;
        s_nvs_namespace_exists = true;
    }
    *handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(
    nvs_handle_t handle,
    const char *key,
    void *out_value,
    size_t *length
)
{
    assert(handle == 1);
    assert(strcmp(key, "client_mac") == 0);
    assert(length != NULL);
    if (s_nvs_get_result != ESP_OK) return s_nvs_get_result;
    if (!s_nvs_blob_exists) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < s_nvs_mac_length) {
        *length = s_nvs_mac_length;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, s_nvs_mac, s_nvs_mac_length);
    *length = s_nvs_mac_length;
    return ESP_OK;
}

esp_err_t nvs_set_blob(
    nvs_handle_t handle,
    const char *key,
    const void *value,
    size_t length
)
{
    assert(handle == 1);
    assert(strcmp(key, "client_mac") == 0);
    assert(value != NULL);
    if (s_nvs_set_result != ESP_OK) return s_nvs_set_result;
    assert(length <= sizeof(s_nvs_mac));
    memcpy(s_nvs_mac, value, length);
    s_nvs_mac_length = length;
    s_nvs_blob_exists = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    ++s_nvs_commit_calls;
    return s_nvs_commit_result;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    assert(handle == 1);
    if (s_nvs_erase_result == ESP_OK) {
        s_nvs_blob_exists = false;
        s_nvs_mac_length = 0U;
    }
    return s_nvs_erase_result;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
    ++s_nvs_close_calls;
}

static void test_credentials_validation_and_round_trip(void)
{
    reset_fakes();
    bool saved = true;
    assert(wifi_manager_credentials_has_saved(NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_credentials_has_saved(&saved) == ESP_OK);
    assert(!saved);
    assert(wifi_manager_credentials_save(NULL, "pw") == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_credentials_save("ssid", NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_credentials_save("", "pw") == ESP_ERR_INVALID_ARG);

    char too_long_ssid[34];
    memset(too_long_ssid, 's', sizeof(too_long_ssid) - 1U);
    too_long_ssid[sizeof(too_long_ssid) - 1U] = '\0';
    assert(wifi_manager_credentials_save(too_long_ssid, "pw") == ESP_ERR_INVALID_ARG);

    char too_long_password[65];
    memset(too_long_password, 'p', sizeof(too_long_password));
    too_long_password[sizeof(too_long_password) - 1U] = '\0';
    assert(wifi_manager_credentials_save("ssid", too_long_password) == ESP_ERR_INVALID_ARG);

    assert(wifi_manager_credentials_save("home", "secret") == ESP_OK);
    assert(s_wifi_set_calls == 1U);
    assert(wifi_manager_credentials_has_saved(&saved) == ESP_OK);
    assert(saved);
    assert(s_wifi_get_calls == 2U);

    char ssid[16];
    assert(wifi_manager_credentials_get_ssid(ssid, sizeof(ssid)) == ESP_OK);
    assert(strcmp(ssid, "home") == 0);
    assert(s_wifi_get_calls == 3U);
    assert(wifi_manager_credentials_get_ssid(NULL, sizeof(ssid)) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_credentials_get_ssid(ssid, 0U) == ESP_ERR_INVALID_ARG);

    s_wifi_get_result = ESP_FAIL;
    assert(wifi_manager_credentials_has_saved(&saved) == ESP_FAIL);
    assert(wifi_manager_credentials_get_ssid(ssid, sizeof(ssid)) == ESP_FAIL);
    s_wifi_set_result = ESP_FAIL;
    assert(wifi_manager_credentials_clear() == ESP_FAIL);
}

static void test_profile_nvs_round_trip_and_shape_errors(void)
{
    reset_fakes();
    const uint8_t mac[6] = { 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U, 0x01U };
    bool known = true;
    assert(wifi_manager_profile_is_known_client(NULL, &known) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_profile_is_known_client(mac, NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_manager_profile_is_known_client(mac, &known) == ESP_OK);
    assert(!known);

    assert(wifi_manager_profile_remember_client(mac) == ESP_OK);
    assert(s_nvs_commit_calls == 1U);
    assert(s_nvs_open_calls == 2U);
    assert(s_nvs_close_calls == 1U);
    assert(wifi_manager_profile_is_known_client(mac, &known) == ESP_OK);
    assert(known);
    assert(s_nvs_open_calls == 3U);
    assert(s_nvs_close_calls == 2U);
    const uint8_t other_mac[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
    assert(wifi_manager_profile_is_known_client(other_mac, &known) == ESP_OK);
    assert(!known);

    s_nvs_mac_length = 5U;
    assert(wifi_manager_profile_is_known_client(mac, &known) == ESP_ERR_INVALID_SIZE);
    s_nvs_mac_length = 6U;
    s_nvs_get_result = ESP_FAIL;
    assert(wifi_manager_profile_is_known_client(mac, &known) == ESP_FAIL);

    s_nvs_get_result = ESP_OK;
    assert(wifi_manager_profile_clear() == ESP_OK);
    assert(!s_nvs_blob_exists);
    assert(wifi_manager_profile_is_known_client(mac, &known) == ESP_OK);
    assert(!known);

    reset_fakes();
    assert(wifi_manager_profile_clear() == ESP_OK);
    s_nvs_open_write_result = ESP_FAIL;
    assert(wifi_manager_profile_clear() == ESP_FAIL);
}

static void test_public_config_wrappers_persist_phone_profile(void)
{
    reset_fakes();
    const uint8_t phone_mac[6] = { 0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U };
    wifi_manager_runtime_set_setup_client(phone_mac, true);
    assert(wifi_manager_save_sta_credentials("field-net", "field-pass") == ESP_OK);
    assert(memcmp(s_wifi_config.sta.ssid, "field-net", 9U) == 0);
    assert(memcmp(s_wifi_config.sta.password, "field-pass", 10U) == 0);

    bool known = false;
    assert(wifi_manager_profile_is_known_client(phone_mac, &known) == ESP_OK);
    assert(known);

    assert(wifi_manager_forget_sta_credentials() == ESP_OK);
    assert(s_wifi_config.sta.ssid[0] == '\0');
    assert(!s_nvs_blob_exists);

    reset_fakes();
    assert(wifi_manager_save_sta_credentials("no-phone", "pw") == ESP_OK);
    assert(!s_nvs_blob_exists);
}

int main(void)
{
    test_credentials_validation_and_round_trip();
    test_profile_nvs_round_trip_and_shape_errors();
    test_public_config_wrappers_persist_phone_profile();
    puts("wifi manager storage tests passed");
    return 0;
}
