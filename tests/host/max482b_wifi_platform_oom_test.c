#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"
#include "wifi_manager_platform.h"

static esp_netif_t s_sta_netif;
static esp_netif_t s_ap_netif;
static bool s_sta_live;
static bool s_ap_live;
static bool s_wifi_live;
static unsigned s_netif_create_calls;
static unsigned s_fail_netif_create_nth;
static unsigned s_wifi_init_calls;
static unsigned s_wifi_deinit_calls;
static unsigned s_wifi_storage_calls;
static esp_err_t s_wifi_init_result;
static esp_err_t s_wifi_storage_result;

static unsigned live_netif_count(void)
{
    return (s_sta_live ? 1U : 0U) + (s_ap_live ? 1U : 0U);
}

static void reset_fakes(void)
{
    assert(live_netif_count() == 0U);
    assert(!s_wifi_live);
    s_netif_create_calls = 0U;
    s_fail_netif_create_nth = 0U;
    s_wifi_init_calls = 0U;
    s_wifi_deinit_calls = 0U;
    s_wifi_storage_calls = 0U;
    s_wifi_init_result = ESP_OK;
    s_wifi_storage_result = ESP_OK;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }

static esp_netif_t *create_netif(esp_netif_t *netif, bool *live)
{
    ++s_netif_create_calls;
    if (s_fail_netif_create_nth == s_netif_create_calls) return NULL;
    assert(!*live);
    *live = true;
    return netif;
}

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return create_netif(&s_sta_netif, &s_sta_live);
}

esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    return create_netif(&s_ap_netif, &s_ap_live);
}

void esp_netif_destroy_default_wifi(void *handle)
{
    if (handle == &s_ap_netif) {
        assert(s_ap_live);
        s_ap_live = false;
    } else {
        assert(handle == &s_sta_netif && s_sta_live);
        s_sta_live = false;
    }
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    assert(config != NULL);
    ++s_wifi_init_calls;
    if (s_wifi_init_result == ESP_OK) s_wifi_live = true;
    return s_wifi_init_result;
}

esp_err_t esp_wifi_deinit(void)
{
    assert(s_wifi_live);
    s_wifi_live = false;
    ++s_wifi_deinit_calls;
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(int storage)
{
    assert(storage == WIFI_STORAGE_FLASH);
    ++s_wifi_storage_calls;
    return s_wifi_storage_result;
}

static void test_setup_failures_roll_back_and_retry(void)
{
    reset_fakes();
    s_fail_netif_create_nth = 2U;
    const esp_err_t ap_create_err = wifi_manager_platform_init();
    assert(live_netif_count() == 0U);
    assert(ap_create_err == ESP_ERR_NO_MEM);

    reset_fakes();
    s_fail_netif_create_nth = 1U;
    assert(wifi_manager_platform_init() == ESP_ERR_NO_MEM);
    assert(live_netif_count() == 0U);

    reset_fakes();
    s_wifi_init_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(live_netif_count() == 0U);
    assert(s_wifi_init_calls == 1U && s_wifi_storage_calls == 0U);

    reset_fakes();
    s_wifi_storage_result = ESP_FAIL;
    assert(wifi_manager_platform_init() == ESP_FAIL);
    assert(live_netif_count() == 0U);
    assert(!s_wifi_live && s_wifi_deinit_calls == 1U);

    reset_fakes();
    assert(wifi_manager_platform_init() == ESP_OK);
    assert(live_netif_count() == 2U);
    assert(wifi_manager_platform_sta_netif() == &s_sta_netif);
    assert(wifi_manager_platform_ap_netif() == &s_ap_netif);
}

int main(void)
{
    test_setup_failures_roll_back_and_retry();
    puts("MAX482B_WIFI_PLATFORM_OOM_PASS");
    return 0;
}
